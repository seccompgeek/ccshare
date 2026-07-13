/*
 * sev_gpu_manager.c
 *
 * SEV GPU Manager - Kernel Module (ivshmem-doorbell backed)
 *
 * Cross-VM GPU sharing for confidential VMs. The shared memory channel is an
 * inter-VM PCI device (QEMU ivshmem-doorbell, 1af4:1110) whose BAR2 is the same
 * physical host RAM mapped into every participating VM. Doorbell MSI-X vectors
 * drive the control plane (request/grant wakeups); the shared BAR carries data.
 *
 * Roles:
 *   - Manager (module param manager=1): owns the protocol, initializes the
 *     shared-memory header, runs the scheduler, grants GPU time.
 *   - Client  (manager=0): writes requests into the shared ring and rings the
 *     manager's doorbell; blocks until its grant arrives.
 *
 * BAR layout:
 *   BAR0 = device registers (IntrMask/IntrStatus/IVPosition/Doorbell)
 *   BAR1 = MSI-X table
 *   BAR2 = shared memory region (header + request + grant + per-VM data)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/timekeeping.h>
#include <linux/io.h>
#include <linux/pgtable.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/scatterlist.h>
#include <crypto/aead.h>
#include <crypto/hash.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>
#include <crypto/algapi.h>
#include <linux/kthread.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_rpc.h"
#include "sev_gpu_crypto.h"
#include "sev_gpu_state.h"
#include "sev_gpu_kmb.h"
#include "sev_gpu_handshake.h"
#include "sev_gpu_client_mmap.h"
#include "sev_gpu_manager_mmap.h"
#include "sev_gpu_comm.h"
#include "sev_gpu_client_rm.h"
#include "sev_gpu_nvidia.h"
#include "sev_gpu_manager_exec.h"
#include "sev_gpu_manager_sched.h"
#include "sev_gpu_bringup.h"
#include "sev_gpu_chardev.h"

static void rpc_client_bind_nvidia(void);
static void rpc_client_unbind_nvidia(void);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martin");
MODULE_DESCRIPTION("SEV GPU Manager - ivshmem-doorbell cross-VM GPU scheduling");
MODULE_VERSION("0.2");

#define DRV_NAME    "sev_gpu_manager"
#define CLASS_NAME  "sev_gpu"


/* Role: manager (default) initializes shared memory and runs the scheduler. */
bool manager = true;
module_param(manager, bool, 0444);
MODULE_PARM_DESC(manager, "1 = act as GPU manager (default), 0 = act as client");

/*
 * RM-RPC loopback (manager only, transport test): when set, the manager echoes
 * each forwarded RM escape back to the client without touching a GPU. Lets the
 * forwarding path be exercised on an ordinary VM with no GPU/nvidia.ko.
 */
bool rpc_loopback;
module_param(rpc_loopback, bool, 0444);
MODULE_PARM_DESC(rpc_loopback, "manager: echo RM-RPC requests without a GPU (transport test)");

/*
 * Manager: complete mediated CE secure-copy requests without a real GPU. The
 * ownership check and offset/alignment framing are still fully enforced; only
 * the GPU Copy Engine submit is skipped (reported as success). Lets the
 * client->manager REQUEST_COPY transport be exercised on a VM with no GPU.
 */
bool copy_loopback;
module_param(copy_loopback, bool, 0444);
MODULE_PARM_DESC(copy_loopback, "manager: complete CE copy requests without a GPU (transport test)");

/*
 * Manager: enforce that a client may only submit work on a channel the manager
 * has assigned to it. The manager is the sole channel allocator; a submitted
 * (hClient, hChannel) must match an entry in this VM's assignment registry or
 * the doorbell is refused (-EACCES). This is the per-VM scoping that closes the
 * cross-VM handle gap in the global replay namespace (g_resServ).
 *
 * Default ON now that compute (GPFIFO) channels are manager-pool-assigned
 * (sev_gpu_assign_compute_channel records every channel in the assignment
 * registry), so submission is scoped to each VM's own channels. Set to 0 only
 * for legacy bring-up where a client still drives a replay-allocated channel
 * that the manager never assigned -- enforcement would otherwise refuse it.
 */
bool enforce_channel_ownership = true;
module_param(enforce_channel_ownership, bool, 0644);
MODULE_PARM_DESC(enforce_channel_ownership,
		 "manager: refuse work-submit on a channel not assigned to the VM (default 1; set 0 for legacy replay bring-up)");

/*
 * Autonomous bring-up doorbell ring (default ON, filtered-safe).
 *
 * The bring-up watcher rings the real GPU usermode doorbell for a channel whose
 * shared-USERD GP_PUT advances. Unmodified CUDA rings the usermode doorbell as a
 * plain memory write (redirected to shadow ivshmem): it raises no interrupt and
 * reaches the manager through no escape/RPC, so polling + ringing here is the
 * ONLY path that propagates a transparent-replay channel's doorbell (such a
 * channel is not in assign_state, so FLUSH_ALL / SUBMIT_WORK cannot reach it).
 *
 * This is SAFE by construction: the ring primitive (rm_sev_gpu_submit_work)
 * refuses to ring a CC-secure channel (returns NV_ERR_NOT_SUPPORTED), so this
 * loop rings ONLY non-secure channels (e.g. the GR compute channel) and no-ops
 * the CC-secure CE/SEC2 ones -- raw-ringing those decrypts stale methods and
 * faults (Xid 71 CC methods -> Xid 154 GPU Reset Required); they need the
 * IV-synced WLC launch instead. Set 0 to disable entirely (e.g. to isolate the
 * WLC path during bring-up).
 */
bool bringup_ring = true;
module_param(bringup_ring, bool, 0644);
MODULE_PARM_DESC(bringup_ring,
		 "manager: (default 1) propagate NON-secure channel doorbells on shared-USERD GP_PUT advance; CC-secure channels are filtered out in the ring primitive (rm_sev_gpu_submit_work)");


/*
 * Manager L1 smoke test (Arch B, compute channels). At bind, allocate, log,
 * and immediately free this many manager-owned compute (GR) GPFIFO channel
 * trees on the real GPU. Proves the manager RM can stand up a GR engine
 * context (client/device/subdevice + VASpace + GR0 TSG) end to end. Off (0) by
 * default; set to a small N (e.g. 1) to run the smoke test from dmesg.
 */
static uint compute_selftest;
module_param(compute_selftest, uint, 0444);
MODULE_PARM_DESC(compute_selftest,
		 "manager: alloc+free N compute (GR) channels at bind as an L1 smoke test (default 0)");

/*
 * Automatic in-kernel KMB handshake. Once the keybroker delivers the comm key
 * (SEV_GPU_IOC_SET_COMM_KEY) the driver itself runs the channel assignment +
 * sealed-KMB exchange -- no manual `kmb_test manager/client` steps. The manager
 * auto-assigns a pre-provisioned channel of hs_keyspace and seals its KMB; the
 * client auto-receives and installs it. Set 0 to drive the exchange by hand.
 */
static bool auto_handshake = true;
module_param(auto_handshake, bool, 0644);
MODULE_PARM_DESC(auto_handshake, "auto-run the KMB exchange after the comm key is set (default 1)");

uint hs_keyspace;
module_param(hs_keyspace, uint, 0644);
MODULE_PARM_DESC(hs_keyspace, "manager: LCE keyspace to auto-assign on handshake (default 0)");

uint hs_timeout_ms = 120000;
module_param(hs_timeout_ms, uint, 0644);
MODULE_PARM_DESC(hs_timeout_ms, "auto-handshake KMB exchange timeout in ms (default 120000)");

/*
 * In-kernel mTLS-equivalent handshake (auto_mtls). Instead of spawning the
 * userspace keybroker, the driver itself performs an authenticated ephemeral
 * ECDHE-PSK key agreement (equivalent to TLS 1.3 psk_dhe_ke) over the ivshmem
 * handshake mailbox on the first client<->manager contact, and installs the
 * resulting AES-256 comm key into comm_keystore. The exchange derives forward
 * secrecy from an ephemeral NIST P-256 ECDH and mutual authentication from a
 * shared 32-byte pre-shared key (psk_path) folded into the HKDF schedule, with
 * explicit HMAC "finished" confirmations so a host that can read or tamper with
 * the shared ring cannot silently MITM the channel. Set 0 to fall back to the
 * external keybroker delivering the key via SEV_GPU_IOC_SET_COMM_KEY.
 */
bool auto_mtls = true;
module_param(auto_mtls, bool, 0644);
MODULE_PARM_DESC(auto_mtls, "run the in-kernel ECDHE-PSK handshake to establish the comm key on first client<->manager contact (default 1)");

static char *psk_path = "/home/martin/sev-gpu/userspace/certs/psk.bin";
module_param(psk_path, charp, 0644);
MODULE_PARM_DESC(psk_path, "path to the 32-byte shared PSK file anchoring the in-kernel handshake");

uint auto_mtls_wait_ms = 4000;
module_param(auto_mtls_wait_ms, uint, 0644);
MODULE_PARM_DESC(auto_mtls_wait_ms, "in-kernel handshake per-phase / comm-key wait in ms (default 4000, keep < RPC timeout)");

/* Per-vector interrupt context (passed as request_irq dev_id). */
/* struct sev_gpu_irq -> sev_gpu_state.h */
/* The shared CONTROL ivshmem device (ivshmem-doorbell: has MSI-X + BAR0 regs). */
/* struct sev_gpu_dev -> sev_gpu_state.h */

/*
 * A PRIVATE per-VM DATA ivshmem device (ivshmem-plain: BAR2 only, no MSI-X).
 * The manager binds one per client; a client binds exactly its own. The
 * region's first page holds a sev_gpu_data_header_t; the payload follows.
 */
/* struct sev_gpu_data_dev -> sev_gpu_state.h */

/* Manager-side bookkeeping. */
struct sev_gpu_manager_state manager_state;

/*
 * Manager<->client communication keys, delivered from userspace after the
 * keybroker's mutual-TLS handshake (SEV_GPU_IOC_SET_COMM_KEY). These never
 * leave the kernel: the driver uses them to seal the in-kernel exchange of the
 * GPU channel key material (CC_KMB) between manager and client. Indexed by
 * vm_id (manager keeps one per client; a client keeps only its own slot).
 */
struct sev_gpu_comm_keystore comm_keystore;

/*
 * Manager-authoritative GPU channel assignment registry (manager only).
 *
 * The manager owns and allocates EVERY GPU channel; clients never allocate or
 * request channels. Each entry records a channel the manager has assigned to a
 * client, together with that channel's KMB (placeholder in D4.1, the real
 * CC_KMB fetched from the manager-owned channel handle in D4.2). KMB delivery
 * is per-assignment: only the assigned channel's KMB is ever sealed to a
 * client -- never "all of them".
 */
/*
 * Must be >= the number of CC-secure channels a single CUDA context brings up.
 * CUDA on Blackwell allocates one channel per copy/compute engine it uses (the
 * proof was observed creating 9+ channels, each needing its own KMB keystore
 * slot); 8 overflowed the client_kmb_store ("GET_KMB install: keystore full").
 * 32 matches SEV_GPU_CC_POOL_MAX and adds only ~0.4 MiB to the per-VM compute
 * reserve (still tiny within the 64 MiB DATA region).
 */

/* Channel kind recorded per assignment (selects the submission datapath). */

/* struct sev_gpu_assignment -> sev_gpu_state.h */

struct sev_gpu_assign_state assign_state;


/*
 * D4.2b provisioner pool: the manager pre-allocates confidential-compute
 * channels on the real GPU (each its own kernel RM client) and hands them out
 * from here on ASSIGN. Clients never allocate channels; the manager is the
 * sole allocator.
 */

/* struct sev_gpu_cc_chan -> sev_gpu_state.h */

struct sev_gpu_cc_pool cc_pool;

/*
 * Client-side installed channel KMBs (D4.3b). A client stores the unsealed
 * CC_KMB for each channel the manager hands it, and uses it to AES-256-GCM the
 * data it stages in the shared bounce region. The KMB never leaves the kernel.
 * Per-bundle message counters feed the CC IV scheme (gcm_iv = counter ^ ivMask).
 */
/* struct sev_client_chan -> sev_gpu_state.h */

struct sev_gpu_client_kmb_store client_kmb_store;

/* AAD that binds each sealed KMB to its (vm, channel, keyspace, seq). */
/* struct sev_gpu_kmb_aad -> sev_gpu_kmb.h */
/* The one control device, and the pool of data devices on this VM. */
struct sev_gpu_dev *ctrl_dev;
struct sev_gpu_data_dev *data_devs[SEV_GPU_MAX_VMS];

/*
 * Automatic KMB-handshake worker. Triggered by SEV_GPU_IOC_SET_COMM_KEY once
 * the keybroker has delivered the comm key: the manager assigns a channel +
 * seals its KMB, the client receives + installs it. Blocking waits (up to
 * hs_timeout_ms) run here, off the ioctl path. One control device per VM, so a
 * single global worker suffices; the manager tracks pending clients in a bitmap.
 */
struct sev_gpu_hs_state hs_state;
int num_data_devs;
/* Client-mode: shadow doorbell PFN stored on MAP_MEMORY reply, consumed by mmap redirect. */
unsigned long doorbell_mmap_pfn;
/*
 * Manager-mode: per-client cache of the client's data-region BAR2 GPA,
 * populated from the RPC slot's client_data_phys field on every request.
 * Avoids depending on the client writing to the shared data-device header
 * (which fails if the client has no data device or if SEV-SNP C-bit issues
 * prevent the data-header write from being visible to the manager).
 */
u64 client_mem_phys_cache[SEV_GPU_MAX_VMS];
/*
 * Manager-mode: per-client bump cursor (region-relative byte offset) for the
 * OS-descriptor reserve. CUDA registers small CPU buffers of its own as GPU
 * memory via esc 0x27 (NV01_MEMORY_SYSTEM_OS_DESCRIPTOR); the manager backs each
 * with a fresh page-aligned slice of that client's ivshmem region carved here.
 * Bump-only (never freed) -- the proof workload registers a handful of buffers.
 */
u64 osdesc_carve_cursor[SEV_GPU_MAX_VMS];
/*
 * DIAG (read-only): region-relative offset of the most recent >=2 MiB osdesc
 * carve per client -- the "0x3e" compute sysmem pool CUDA faults on. Lets the
 * manager replay path log its OWN copy of the control word at pool+0x3fffc so
 * we can tell whether the real driver ever writes it (coherency vs bring-up).
 */
u64 osdesc_2m_off[SEV_GPU_MAX_VMS];
/*
 * DIAG (read-only): region-relative offset of the channel USERD/GPFIFO buffer
 * -- the FIRST >=2 MiB osdesc carve of a run, backed shared for the client
 * (Task A). Because the ring/USERD are now CPU-visible to the manager, the
 * replay path can inspect whether CUDA ever writes a GPFIFO entry + GP_PUT into
 * it, i.e. whether it attempts a work submit at all. Latched on first 2 MiB
 * carve, cleared on pool reclaim.
 */
u64 userd_2m_off[SEV_GPU_MAX_VMS];
DEFINE_MUTEX(reg_lock);

/* Shared chardev infrastructure: minor 0 = control, minors 1.. = data. */
#define SEV_GPU_MINORS	(1 + SEV_GPU_MAX_VMS)
struct class *sev_gpu_class;
dev_t sev_gpu_devt_base;

/* ------------------------------------------------------------------ */
/* RM-RPC (Phase D1): control-plane forwarding of nvidia RM escape ioctls */
/* ------------------------------------------------------------------ */

/* Forwarder installed into nvidia.ko's escape hook (client side). */
/* sev_gpu_rm_forwarder_t -> sev_gpu_client_rm.h */
/* Replay handler exported by nvidia.ko to run an escape on the real GPU. */
/* CC_KMB fetch exported by nvidia.ko to read a channel's key bundle (D4.2). */
typedef u32 (*sev_gpu_kmb_fetch_t)(u32 h_client, u32 h_channel,
				   void *kmb_out, u32 kmb_size);
/* CC-channel pool allocator/free exported by nvidia.ko (D4.2b provisioner). */
/* sev_gpu_ce_submit_t -> sev_gpu_manager_exec.h */
/* sev_gpu_submit_work_t -> sev_gpu_manager_exec.h */
/* Compute work-submit-token query exported by nvidia.ko (Fork B WLC launch). */
/* Compute (GR) GPFIFO channel pool alloc/free exported by nvidia.ko (Arch B). */
/*
 * Confidential CC_KMB seal (manager) / install (client) callbacks registered
 * INTO nvidia.ko. On GET_KMB the manager fetches the raw KMB from the GPU and
 * calls the seal callback to encrypt it under the per-client comm key before it
 * crosses host-visible ivshmem; the client calls the install callback to unseal
 * it and stash it in the per-channel keystore. Signatures mirror nv.c exactly.
 */
/* sev_gpu_kmb_seal_t / _install_t typedefs -> sev_gpu_kmb.h */

#define RPC_STATE_OFF	offsetof(sev_gpu_rpc_slot_t, state)
#define RPC_TIMEOUT_MS	5000		/* client: max wait for a reply           */

/*
 * Base offset (from the start of a per-VM data region) of the zero-copy
 * nested-param staging window -- the last SEV_GPU_RPC_DATA_STAGING_SIZE bytes of
 * the region. Both peers map the same region with the same size, so they agree
 * on the base. Returns 0 if the region is too small to host the window.
 */
/*
 * Region geometry (rpc_staging_base, compute_reserve_base, compute_doorbell_off,
 * osdesc_reserve_base, wlc_lcic_reserve_base and the reserve-size #defines)
 * moved to sev_gpu_regions.h — the single source of truth for BAR2 offsets,
 * shared by both roles and both transports.
 */
#include "sev_gpu_regions.h"

/*
 * Client: hand nvidia-uvm.ko this VM's OWN view of its WLC/LCIC reserve band and
 * shadow doorbell, so nvidia-uvm can build its proxy WLC/LCIC pools mapped onto the
 * shared ivshmem band -- the exact bytes the manager's paired GPU channels DMA. Both
 * peers derive the same region-relative offsets from the shared reserve geometry
 * (wlc_lcic_reserve_base / compute_doorbell_off); the client supplies its data-region
 * base (data_devs[0]->mem_phys). Bound by nvidia-uvm via symbol_get at client GPU
 * bring-up (no hard module dep). Returns 0 on success, negative errno otherwise.
 *
 * Client-role only: the manager builds REAL per-client pools over each client's
 * manager-view GPA (sev_gpu_manager_setup_client_channels), never a proxy over its
 * own region, so reject the manager role outright.
 */
int sev_gpu_client_reserve_band(u64 *wlc_gpa, u64 *wlc_size,
				u64 *lcic_gpa, u64 *lcic_size, u64 *doorbell_gpa);
int sev_gpu_client_reserve_band(u64 *wlc_gpa, u64 *wlc_size,
				u64 *lcic_gpa, u64 *lcic_size, u64 *doorbell_gpa)
{
	struct sev_gpu_data_dev *dd;
	u64 band, db;

	if (manager)			/* manager role builds real pools, not proxies */
		return -EPERM;
	if (num_data_devs < 1 || !data_devs[0])
		return -ENODEV;
	dd = data_devs[0];		/* the client has exactly one data region */
	if (!dd->mem_phys || dd->is_manager)
		return -ENODEV;

	band = wlc_lcic_reserve_base(dd->mem_size);
	db = compute_doorbell_off(dd->mem_size);
	if (!band || !db)
		return -ENOSPC;		/* region too small for the reserves */

	if (wlc_gpa)
		*wlc_gpa = (u64)dd->mem_phys + band;
	if (wlc_size)
		*wlc_size = SEV_GPU_WLC_SYSMEM_SIZE;
	if (lcic_gpa)
		*lcic_gpa = (u64)dd->mem_phys + band + SEV_GPU_WLC_SYSMEM_SIZE;
	if (lcic_size)
		*lcic_size = SEV_GPU_LCIC_SYSMEM_SIZE;
	if (doorbell_gpa)
		*doorbell_gpa = (u64)dd->mem_phys + db;

	pr_info("sev_gpu: client reserve band wlc_gpa=0x%llx lcic_gpa=0x%llx doorbell_gpa=0x%llx\n",
		(u64)dd->mem_phys + band,
		(u64)dd->mem_phys + band + SEV_GPU_WLC_SYSMEM_SIZE,
		(u64)dd->mem_phys + db);
	return 0;
}
EXPORT_SYMBOL_GPL(sev_gpu_client_reserve_band);

/*
 * Carve slot @idx's USERD/GPFIFO out of @dd's private data region. Fills the
 * GPU-DMA-able physical addresses (userd_gpa, gpfifo_gpa == mem_phys + region
 * offset, the same precedent as sev_gpu_do_ce_copy) and the region-relative
 * offsets the client uses to map them. Returns 0 or a negative errno.
 */
int sev_gpu_compute_carve(struct sev_gpu_data_dev *dd, u32 idx,
				 u64 *userd_gpa, u64 *gpfifo_gpa,
				 u64 *pushbuf_gpa, u64 *userd_off,
				 u64 *gpfifo_off, u64 *pushbuf_off,
				 u64 *enc_gpa, u64 *enc_off)
{
	u64 base, u_off, g_off, p_off, e_off;

	if (!dd || !dd->mem_phys || idx >= SEV_GPU_MAX_CHANNELS_PER_VM)
		return -EINVAL;

	base = compute_reserve_base(dd->mem_size);
	if (!base)
		return -ENOSPC;	/* region too small for the compute reserve */

	u_off = base + (u64)idx * SEV_GPU_COMPUTE_CHAN_STRIDE;
	g_off = u_off + PAGE_SIZE;
	p_off = g_off + PAGE_SIZE;
	e_off = p_off + PAGE_SIZE;			/* enc: cipher page + tag page */
	if (e_off + 2 * PAGE_SIZE > dd->mem_size)
		return -ENOSPC;

	if (!IS_ALIGNED((u64)dd->mem_phys + u_off, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)dd->mem_phys + g_off, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)dd->mem_phys + p_off, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)dd->mem_phys + e_off, PAGE_SIZE))
		return -EINVAL;

	*userd_gpa    = (u64)dd->mem_phys + u_off;
	*gpfifo_gpa   = (u64)dd->mem_phys + g_off;
	*pushbuf_gpa  = (u64)dd->mem_phys + p_off;
	*userd_off    = u_off;
	*gpfifo_off   = g_off;
	*pushbuf_off  = p_off;
	if (enc_gpa)
		*enc_gpa = (u64)dd->mem_phys + e_off;
	if (enc_off)
		*enc_off = e_off;
	return 0;
}

/*
 * NV escape command keys (low 8 bits of the ioctl) and the field offsets of the
 * nested-pointer-bearing NVOSxx params we deep-copy. Mirrored here so the
 * transport stays free of the NVIDIA SDK headers; these MUST track:
 *   nv_escape.h          NV_ESC_RM_{FREE,CONTROL,ALLOC}
 *   nvos.h               NVOS54_PARAMETERS / NVOS21_PARAMETERS / NVOS64_PARAMETERS
 *   rs_access.h          RS_ACCESS_MASK (1 x NvU32 limb)
 * All offsets are LP64 with NvP64 8-byte aligned (the SDK uses NV_ALIGN_BYTES(8)).
 */
#define RPC_NV_ESC_RM_FREE	0x29u

/*
 * One nested pointer found inside a top-level escape struct: the 8-byte NvP64
 * field at @ptr_off references @size bytes that must be deep-copied in @dir.
 * Only the CLIENT needs this per-cmd knowledge -- it builds the buffers[] table
 * the (cmd-agnostic) manager then replays.
 */
/* struct rpc_nested -> sev_gpu_rpc.h */
/*
 * Per-RM-control forwarding policy (client side). Most controls are FLAT (no
 * embedded pointers) and ride the flat/FINN path. A control whose pParams embeds
 * further pointers needs explicit level-2 staging (the manager re-points each
 * embedded pointer at the shared staging region before replay). A few controls
 * could instead be answered locally by the client module from a value the
 * manager publishes into shared memory -- RPC_CTRL_LOCAL is the (currently
 * unused) hook for that, so the client does not blindly forward everything.
 */
/* enum rpc_ctrl_disp -> sev_gpu_comm.h */

/*
 * One embedded pointer field inside a control's pParams struct.
 *  pptr_off  - byte offset of the 8-byte NvP64 field within pParams.
 *  size_off  - byte offset of the u32 length field within pParams that sizes
 *              this buffer, or RPC_SIZE_FIXED to use @fixed_size.
 *  fixed_size- used when size_off == RPC_SIZE_FIXED.
 *  dir       - SEV_GPU_RPC_BUF_* copy direction of the pointee.
 *  elem_size - if non-zero, the u32 field at @size_off is an element COUNT and
 *              the pointee byte length is count * elem_size; if zero, the field
 *              at @size_off already holds a byte length.
 */
/* struct rpc_embedded_field -> sev_gpu_comm.h */

/* struct rpc_ctrl_policy -> sev_gpu_comm.h */

/*
 * GET_BUILD_VERSION: pParams embeds three OUT string buffers (driver/version/
 * title), each sizeOfStrings (u32 @ offset 0) bytes.
 */
static const struct rpc_embedded_field bv_fields[] = {
	{ BVPAR_PDRVVER_OFF, BVPAR_SOS_OFF, 0, SEV_GPU_RPC_BUF_OUT, 0 },
	{ BVPAR_PVER_OFF,    BVPAR_SOS_OFF, 0, SEV_GPU_RPC_BUF_OUT, 0 },
	{ BVPAR_PTITLE_OFF,  BVPAR_SOS_OFF, 0, SEV_GPU_RPC_BUF_OUT, 0 },
};

/*
 * NV2080_CTRL_CMD_GR_GET_INFO (0x20801201): pParams (NV2080_CTRL_GR_GET_INFO_
 * PARAMS) embeds one INOUT list pointer. Layout (LP64, 8-byte aligned NvP64):
 *   [0]  grInfoListSize  NvU32  number of NV2080_CTRL_GR_INFO entries
 *   [8]  grInfoList      NvP64  IN: caller's query indices / OUT: returned data
 * Each NV2080_CTRL_GR_INFO is { NvU32 index; NvU32 data; } = 8 bytes.
 */
#define NV2080_CTRL_CMD_GR_GET_INFO   0x20801201u
#define GRINFO_LISTSIZE_OFF   0u   /* grInfoListSize (entry count) */
#define GRINFO_LIST_OFF       8u   /* grInfoList (NvP64)           */
#define GRINFO_ELEM_SIZE      8u   /* sizeof(NV2080_CTRL_GR_INFO)  */

static const struct rpc_embedded_field gr_get_info_fields[] = {
	{ GRINFO_LIST_OFF, GRINFO_LISTSIZE_OFF, 0, SEV_GPU_RPC_BUF_INOUT,
	  GRINFO_ELEM_SIZE },
};

/*
 * NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST (0x80170d): pParams
 * (NV0080_CTRL_FIFO_GET_CHANNELLIST_PARAMS) embeds two count-sized pointers,
 * both sized by numChannels. Layout (LP64, 8-byte aligned NvP64):
 *   [0]  numChannels          NvU32  number of channels queried
 *   [8]  pChannelHandleList    NvP64  IN:  caller's channel handles
 *   [16] pChannelList          NvP64  OUT: returned hardware channel IDs
 * Each list element is a NvU32 (4 bytes). This control is ROUTE_TO_PHYSICAL
 * (goes to GSP), so it must ride the legacy transport -- the manager's RM
 * FINN-serializes it itself; a client-pre-serialized blob fails the GSP
 * round-trip with NV_ERR_INVALID_ARGUMENT (0x1f), aborting channel bring-up.
 */
#define NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST 0x80170du
#define CHLIST_NUMCH_OFF   0u   /* numChannels (entry count)       */
#define CHLIST_HLIST_OFF   8u   /* pChannelHandleList (NvP64, IN)  */
#define CHLIST_CLIST_OFF  16u   /* pChannelList (NvP64, OUT)       */
#define CHLIST_ELEM_SIZE   4u   /* sizeof(NvU32)                   */

static const struct rpc_embedded_field get_channellist_fields[] = {
	{ CHLIST_HLIST_OFF, CHLIST_NUMCH_OFF, 0, SEV_GPU_RPC_BUF_IN,
	  CHLIST_ELEM_SIZE },
	{ CHLIST_CLIST_OFF, CHLIST_NUMCH_OFF, 0, SEV_GPU_RPC_BUF_OUT,
	  CHLIST_ELEM_SIZE },
};

/* Keep RPC_CTRL_LEVEL2 entries in sync with nv.c sev_finn_embedded_ptr_ctrls[]. */
static const struct rpc_ctrl_policy rpc_ctrl_policies[] = {
	{ NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION, RPC_CTRL_LEVEL2,
	  ARRAY_SIZE(bv_fields), bv_fields },
	{ NV2080_CTRL_CMD_GR_GET_INFO, RPC_CTRL_LEVEL2,
	  ARRAY_SIZE(gr_get_info_fields), gr_get_info_fields },
	{ NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST, RPC_CTRL_LEVEL2,
	  ARRAY_SIZE(get_channellist_fields), get_channellist_fields },
};

const struct rpc_ctrl_policy *rpc_ctrl_policy(u32 ctrl_cmd)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(rpc_ctrl_policies); i++)
		if (rpc_ctrl_policies[i].ctrl_cmd == ctrl_cmd)
			return &rpc_ctrl_policies[i];
	return NULL;
}

/*
 * Describe the nested pointers carried by @cmd's top-level escape struct @arg
 * (a kernel copy of arg_size bytes). Returns the descriptor count (0 == flat),
 * or -1 if the cmd carries nested pointers this first cut cannot marshal. The
 * caller skips any descriptor whose in-struct pointer is NULL.
 */
int rpc_nested_layout(u32 cmd, const void *arg, u32 arg_size,
			     struct rpc_nested out[SEV_GPU_RPC_MAX_BUFFERS])
{
	int n = 0;

	switch (cmd) {
	case RPC_NV_ESC_RM_CONTROL: {		/* NVOS54_PARAMETERS */
		u32 psz = 0;

		if (arg_size < RPC_NVOS54_SIZE)
			return -1;
		memcpy(&psz, (const u8 *)arg + RPC_NVOS54_PARAMSSIZE_OFF, 4);
		out[n++] = (struct rpc_nested){ RPC_NVOS54_PARAMS_OFF, psz,
						SEV_GPU_RPC_BUF_INOUT };
		return n;
	}
	case RPC_NV_ESC_RM_ALLOC: {		/* NVOS21_PARAMETERS / NVOS64_PARAMETERS */
		u32 psz = 0;

		if (arg_size == RPC_NVOS64_SIZE) {
			memcpy(&psz, (const u8 *)arg + RPC_NVOS64_PARAMSSIZE_OFF, 4);
			/*
			 * CC mode zeros NVOS64.paramsSize for several alloc classes while
			 * still populating the struct at pAllocParms.  Recover the known
			 * size so the staging path below actually copies the params to the
			 * data region and the manager's re-pointer can make them available.
			 *   0x3e  NV01_MEMORY_SYSTEM / 0x40 NV01_MEMORY_LOCAL_USER
			 *   0x50a0 NV50_MEMORY_VIRTUAL
			 *         -> sizeof(NV_MEMORY_ALLOCATION_PARAMS)            = 128
			 *   0xa06c KEPLER_CHANNEL_GROUP_A (TSG)
			 *         -> sizeof(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS) =  20
			 *            { hObjectError, hObjectEccError, hVASpace, engineType
			 *              (4x u32) + bIsCallingContextVgpuPlugin (NvBool, pad) }.
			 * Without the TSG params the manager replays a NULL channel-group
			 * alloc and RM rejects it NV_ERR_INVALID_ARGUMENT (0x1f), aborting
			 * CUDA's channel/context creation on the first cudaMalloc.
			 */
			if (psz == 0) {
				u32 hClass = 0;
				memcpy(&hClass, (const u8 *)arg + 12, 4);
				if (hClass == 0x0040u || hClass == 0x003eu ||
				    hClass == 0x50a0u)
					psz = 128; /* sizeof(NV_MEMORY_ALLOCATION_PARAMS) */
				else if (hClass == 0xa06cu)
					psz = 20;  /* sizeof(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS) */
				else if (hClass == 0x0079u)
					psz = 24;  /* sizeof(NV0005_ALLOC_PARAMETERS); NV01_EVENT_OS_EVENT */
				else if (hClass == 0x9067u)
					psz = 12;  /* sizeof(NV_CTXSHARE_ALLOCATION_PARAMETERS); FERMI_CONTEXT_SHARE_A */
				else if ((hClass & 0xffu) == 0x6fu &&
					 (hClass >> 8) >= 0xc3u && (hClass >> 8) <= 0xc9u)
					psz = 368; /* sizeof(NV_CHANNEL_ALLOC_PARAMS); GPFIFO channel */
			}
			out[n++] = (struct rpc_nested){ RPC_NVOS21_PALLOC_OFF, psz,
							SEV_GPU_RPC_BUF_INOUT };
			out[n++] = (struct rpc_nested){ RPC_NVOS64_PRIGHTS_OFF,
							RPC_RS_ACCESS_MASK_SIZE,
							SEV_GPU_RPC_BUF_IN };
			return n;
		}
		if (arg_size == RPC_NVOS21_SIZE) {
			memcpy(&psz, (const u8 *)arg + RPC_NVOS21_PARAMSSIZE_OFF, 4);
			out[n++] = (struct rpc_nested){ RPC_NVOS21_PALLOC_OFF, psz,
							SEV_GPU_RPC_BUF_INOUT };
			return n;
		}
		return -1;
	}
	default:
		return 0;		/* flat escape: no nested pointers */
	}
}

DEFINE_MUTEX(rpc_client_lock);
u32 rpc_client_seq;
static struct task_struct *rpc_kthread;	/* manager: mailbox replay poller          */
DECLARE_WAIT_QUEUE_HEAD(rpc_wq);
atomic_t rpc_kick = ATOMIC_INIT(0);
sev_gpu_rm_replay_t rpc_replay_fn;	/* manager: bound from nvidia.ko    */
sev_gpu_kmb_fetch_t kmb_fetch_fn;	/* manager: bound from nvidia.ko    */
sev_gpu_chan_alloc_t chan_alloc_fn;	/* manager: bound from nvidia.ko    */
sev_gpu_chan_free_t chan_free_fn;	/* manager: bound from nvidia.ko    */
sev_gpu_ce_submit_t ce_submit_fn;	/* manager: bound from nvidia.ko    */
sev_gpu_submit_work_t submit_work_fn;	/* manager: bound from nvidia.ko    */
sev_gpu_get_work_submit_token_t get_work_submit_token_fn; /* bound from nvidia.ko */
sev_gpu_compute_alloc_t compute_alloc_fn;	/* manager: bound from nvidia.ko */
sev_gpu_compute_free_t compute_free_fn;	/* manager: bound from nvidia.ko    */
static void (*rpc_replay_teardown)(void);	/* manager: nvidia.ko replay teardown */
static void (*rpc_unregister_forwarder)(void);	/* client: nvidia.ko unbind        */





/* ------------------------------------------------------------------ */
/* Shared-memory helpers                                               */
/* ------------------------------------------------------------------ */



/* Manager: lay out and publish the shared-memory header. */
static void manager_init_layout(struct sev_gpu_dev *d)
{
	sev_gpu_shmem_header_t h;

	memset(&h, 0, sizeof(h));
	h.magic   = SHMEM_MAGIC;
	h.version = 1;
	h.num_vms = 0;

	h.request_region_off  = SEV_GPU_HEADER_SIZE;
	h.request_region_size = SEV_GPU_REQUEST_REGION_SIZE;
	h.grant_region_off    = h.request_region_off + SEV_GPU_REQUEST_REGION_SIZE;
	h.grant_region_size   = SEV_GPU_GRANT_REGION_SIZE;

	/*
	 * The control region carries ONLY scheduling metadata now; GPU-work
	 * staging lives in separate, hardware-isolated per-VM data devices.
	 * So the data-region fields are left zero here.
	 */
	h.data_region_off  = 0;
	h.data_region_size = 0;

	h.request_notify_fd = -1;
	h.grant_notify_fd   = -1;

	/*
	 * Publish our ivshmem peer id so clients ring the correct doorbell
	 * regardless of VM launch order (manager need not be IVPosition 0).
	 */
	h.manager_peer_id = (u32)d->ivposition;

	memcpy_toio(d->shmem, &h, sizeof(h));

	d->request_off = h.request_region_off;
	d->grant_off   = h.grant_region_off;
	d->data_off    = 0;
	d->per_vm_data = 0;

	/*
	 * Initialise the per-VM RM-RPC mailbox slots (control BAR) to IDLE so the
	 * replay thread never mistakes uninitialised shared memory for a pending
	 * request. Warn if the BAR is too small to host the mailbox region.
	 */
	if (SEV_GPU_RPC_CTRL_REGION_OFF +
	    (size_t)SEV_GPU_MAX_VMS * SEV_GPU_RPC_SLOT_STRIDE > d->shmem_size) {
		pr_warn("sev_gpu: control BAR too small for RM-RPC mailbox (%zu < %lu)\n",
			d->shmem_size,
			SEV_GPU_RPC_CTRL_REGION_OFF +
			(unsigned long)SEV_GPU_MAX_VMS * SEV_GPU_RPC_SLOT_STRIDE);
	} else {
		int vm;

		for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++)
			iowrite32(SEV_GPU_RPC_IDLE,
				  d->shmem + SEV_GPU_RPC_CTRL_REGION_OFF +
				  (size_t)vm * SEV_GPU_RPC_SLOT_STRIDE + RPC_STATE_OFF);
	}

	/*
	 * Initialise the per-VM sealed-KMB mailbox slots (control BAR) to IDLE so
	 * a client never mistakes uninitialised shared memory for a posted KMB.
	 */
	if (SEV_GPU_KMB_REGION_OFF + SEV_GPU_KMB_REGION_SIZE > d->shmem_size) {
		pr_warn("sev_gpu: control BAR too small for KMB mailbox (%zu < %lu)\n",
			d->shmem_size,
			(unsigned long)(SEV_GPU_KMB_REGION_OFF +
					SEV_GPU_KMB_REGION_SIZE));
	} else {
		int vm;

		for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++)
			iowrite32(SEV_GPU_KMB_IDLE,
				  d->shmem + SEV_GPU_KMB_REGION_OFF +
				  (size_t)vm * SEV_GPU_KMB_SLOT_STRIDE +
				  offsetof(struct sev_gpu_kmb_mbox, state));
	}

	pr_info("sev_gpu: control layout req@0x%llx grant@0x%llx tls@0x%lx rpc@0x%lx (data in private devs)\n",
		h.request_region_off, h.grant_region_off,
		(unsigned long)SEV_GPU_TLS_REGION_OFF,
		(unsigned long)SEV_GPU_RPC_CTRL_REGION_OFF);
}

/* Client: read the layout the manager published. Returns -EAGAIN if not ready. */
static int client_read_layout(struct sev_gpu_dev *d)
{
	sev_gpu_shmem_header_t h;

	memcpy_fromio(&h, d->shmem, sizeof(h));
	if (h.magic != SHMEM_MAGIC)
		return -EAGAIN;

	d->request_off = h.request_region_off;
	d->grant_off   = h.grant_region_off;
	d->data_off    = h.data_region_off;
	d->per_vm_data = h.data_region_size;
	pr_info("sev_gpu: read manager layout magic=0x%llx ver=%u req@0x%llx grant@0x%llx data@0x%llx per_vm=%u mgr_peer=%u\n",
		h.magic, h.version, h.request_region_off,
		h.grant_region_off, h.data_region_off, h.data_region_size,
		h.manager_peer_id);
	return 0;
}



/*
 * Interrupt-mitigation helpers (NAPI-style). MSI-X vectors are masked at the
 * IRQ layer with disable_irq/enable_irq; the ivshmem IntrMask register only
 * gates legacy INTx, so it is not used here.
 */




/* Client-side predicate: has our grant arrived? */
static bool grant_ready(struct sev_gpu_dev *d, u8 vm)
{
	gpu_grant_t g;

	memcpy_fromio(&g, grant_slot(d, vm), sizeof(g));
	return g.status == GPU_STATUS_GRANTED && g.vm_id == vm;
}

/* ------------------------------------------------------------------ */
/* Manager scheduler                                                   */
/* ------------------------------------------------------------------ */



/*
 * Manager per-client UVM channel pools -- provided by nvidia-uvm.ko, bound via
 * symbol_get (no compile-time dependency, mirroring how this module binds
 * nvidia.ko's sev_gpu_* hooks). uvm_sev_manager_create_client_pool() builds the
 * manager's resident UVM channel manager on the first client (the manager runs
 * no CUDA of its own) and adds one per-client CE pool on an idle Copy Engine;
 * uvm_sev_manager_release_gpu() drops one client's hold. The GPU UUID is passed
 * as a raw 16-byte pointer, ABI-compatible with the callee's NvProcessorUuid*.
 */
extern u32 uvm_sev_manager_create_client_pool(const void *gpu_uuid, u32 client_id,
					      u64 wlc_gpa, u64 wlc_size,
					      u64 lcic_gpa, u64 lcic_size);

/* The real GPU's UUID, captured when the identity is published (see
 * sev_gpu_publish_gpu_desc), and a bitmap of clients whose per-client pool the
 * manager has already created. */
u8 manager_gpu_uuid[16];
bool manager_gpu_uuid_valid;
unsigned long client_channels_setup;
unsigned long client_channels_pending;


static DECLARE_WORK(client_setup_work, sev_gpu_manager_setup_work_fn);

/*
 * Manager: note that a client's comm channel + KMB are established, and queue its
 * per-client UVM channel setup on the workqueue. Idempotent (a client already set
 * up or already queued is ignored). Called from sev_gpu_commit_comm_key(), so it
 * runs after authentication -- never on unauthenticated input.
 */
void sev_gpu_manager_note_client_active(u32 vm_id)
{
	if (!manager || vm_id >= SEV_GPU_MAX_VMS)
		return;
	if (!READ_ONCE(manager_gpu_uuid_valid))
		return;
	if (test_bit(vm_id, &client_channels_setup))
		return;				/* already built */
	if (test_and_set_bit(vm_id, &client_channels_pending))
		return;				/* already queued */
	schedule_work(&client_setup_work);
}



/* ------------------------------------------------------------------ */
/* RM-RPC mailbox transport (manager replay side)                      */
/* ------------------------------------------------------------------ */





/*
 * Bring-up doorbell propagation (Option C).
 *
 * When unmodified CUDA brings a replay-allocated GPFIFO channel up, it submits
 * the bring-up pushbuffer by writing GP_PUT into USERD and ringing the usermode
 * doorbell (NVC361_NOTIFY_CHANNEL_PENDING, page offset +0x90). Its MAP_MEMORY
 * of the *_USERMODE_A object was redirected to the client's SHADOW ivshmem page
 * (see nv.c), so that write lands in plain shared RAM -- which raises no MSI-X.
 * The manager therefore gets NO interrupt for the ring and never pokes the real
 * GPU, so the channel is runnable but never runs and the bring-up completion
 * semaphore the client polls (pool+0x3fffc) stays 0 -> CUDA aborts.
 *
 * The only event-free way to observe a plain memory write is to poll it. When a
 * channel-class alloc is replayed we ARM a per-VM watch; the replay poller then
 * samples the shadow doorbell word for a bounded window and, on an advance,
 * rings the real doorbell on the client's behalf (the same primitive the STAGED
 * flush uses, minus the client-driven ownership gate: the handles here are the
 * manager's own from replay). This is diagnostic as well as corrective -- it
 * logs whether/when the token moves and what the completion semaphore reads.
 */
/*
 * USERD ring-pointer offsets (NV_RAMUSERD_*, dev_ram.h -> dword*4): GP_GET is
 * dword 34 (0x88) and GP_PUT is dword 35 (0x8C). CUDA advances GP_PUT@0x8C when
 * it queues a GPFIFO entry -- this is the ring producer, distinct from the
 * usermode doorbell kick at NVC361_NOTIFY_CHANNEL_PENDING (0x90). Logging both
 * from the USERD region reveals whether CUDA published work (GP_PUT moved) even
 * when the +0x90 usermode doorbell never fires (Hopper+/Blackwell CC channels
 * kick via BAR0 NV_VIRTUAL_FUNCTION_DOORBELL 0x30090 instead).
 */

/*
 * GP_PUT-advance doorbell ring (experiment).
 *
 * A replay-allocated GPFIFO channel whose USERD lives in the shared 0x3e pool
 * (observed: the CE / engine-0x31 channel, userd_shared=1, USERD at buf+0x2000)
 * lets the manager SEE the client advance GP_PUT@USERD+0x8C when it queues
 * GPFIFO entries. The compute channels keep USERD in vidmem (invisible), but the
 * work that actually needs the doorbell rung is queued on the shared-USERD
 * channel. When GP_PUT advances past GP_GET we ring EVERY replay channel's real
 * doorbell (harmless for idle channels: GET==PUT -> host no-op) so the GPU
 * consumes the queued entries. The single-slot +0x90 watch above is retained
 * for diagnostics; these per-VM sets ensure the shared-USERD channel (which may
 * have been armed earlier and overwritten) is still rung.
 */

/* struct sev_gpu_bringup_watch -> sev_gpu_bringup.h */

/* Per-VM set of every replay GPFIFO channel armed (dedup), for GP_PUT ringing. */
/* Per-VM set of >=2 MiB 0x3e carve region offsets (shared-USERD candidates). */
u64 bringup_userd_cand[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
u32 bringup_cand_lastput[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
u32 bringup_cand_lastget[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
u32 bringup_ncand[SEV_GPU_MAX_VMS];





/*
 * Manager bottom half: scan every bound client data region for a pending
 * RM-RPC request and service it. One request in flight per client.
 */
static void sev_gpu_rpc_work(struct work_struct *w)
{
	int i;

	pr_info_ratelimited("sev_gpu: rpc_work: scanning %d data region(s)\n",
			    num_data_devs);

	for (i = 0; i < num_data_devs; i++) {
		struct sev_gpu_data_dev *dd = data_devs[i];
		void __iomem *mb;
		u64 magic;
		u32 state;

		if (!dd || !dd->mem)
			continue;

		/* Mediated CE copy request: lives in the data-region header. */
		sev_gpu_copy_service(dd, (u32)dd->pool_index);

		if (dd->mem_size < SEV_GPU_RPC_STAGING_OFF)
			continue;

		mb = dd->mem + SEV_GPU_RPC_MAILBOX_OFF;
		memcpy_fromio(&magic, mb + offsetof(sev_gpu_rpc_slot_t, magic),
			      sizeof(magic));
		if (magic != SEV_GPU_RPC_MAGIC)
			continue;

		state = ioread32(mb + offsetof(sev_gpu_rpc_slot_t, state));
		if (state != SEV_GPU_RPC_REQUEST)
			continue;

		sev_gpu_rpc_service(dd);
	}
}

/* ------------------------------------------------------------------ */
/* Interrupt handling                                                  */
/* ------------------------------------------------------------------ */

static irqreturn_t sev_gpu_irq_handler(int irq, void *data)
{
	struct sev_gpu_irq *ctx = data;
	struct sev_gpu_dev *d = ctx->dev;

	switch (ctx->vector) {
	case IVSHMEM_VECTOR_NEW_REQUEST:
	case IVSHMEM_VECTOR_RELEASE:
		if (d->is_manager && d->sched_wq) {
			/* Enter poll mode on the first interrupt: mask the
			 * request/release vectors so a burst of client
			 * doorbells doesn't storm us. The poller re-arms them
			 * once the queue drains. */
			if (atomic_cmpxchg(&d->mgr_polling, 0, 1) == 0)
				mgr_irq_mask(d);
			queue_work(d->sched_wq, &d->sched_work);
		}
		break;
	case IVSHMEM_VECTOR_GRANT_READY:
		if (!d->is_manager) {
			/* First grant interrupt switches the waiter to polling;
			 * mask this vector until WAIT_GRANT re-arms it. */
			if (atomic_cmpxchg(&d->cli_polling, 0, 1) == 0)
				disable_irq_nosync(pci_irq_vector(d->pdev,
						IVSHMEM_VECTOR_GRANT_READY));
			wake_up_interruptible(&d->grant_wq);
		}
		break;
	case IVSHMEM_VECTOR_RPC:
		if (d->is_manager) {
			pr_info_ratelimited("sev_gpu: RPC doorbell -> queue rpc_work\n");
			/* Wake the RM-RPC replay kthread immediately so a
			 * forwarded escape isn't delayed up to RPC_IDLE_POLL_MS.
			 * The data-region rpc_work scan and the control-mailbox
			 * kthread are separate; both must be kicked. */
			rpc_wake_manager();
			if (d->rpc_wq)
				queue_work(d->rpc_wq, &d->rpc_work);
		} else {
			/* Client polls the mailbox; wake any sleeper early. */
			wake_up_interruptible(&d->grant_wq);
		}
		break;
	default:
		break;
	}
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Character device                                                    */
/* ------------------------------------------------------------------ */





/*
 * Client: ask the manager to launch a CE secure-copy on a channel we own. A
 * client cannot ring a GPU doorbell, so it stages the submit parameters in its
 * PRIVATE data-region header, flips the state to SEV_GPU_DATA_STAGED, kicks the
 * manager and blocks for the result. Serialized so only one job is in flight.
 */
static long sev_gpu_request_copy(struct sev_gpu_dev *d, void __user *argp)
{
	struct sev_gpu_data_dev *dd = data_devs[0];	/* client: single region */
	void __iomem *hdr;
	sev_gpu_ioctl_submit_t su;
	unsigned long deadline;
	u32 state;
	s32 status;
	u64 magic;
	long ret = 0;

	if (d->is_manager)
		return -EPERM;			/* run on a client (manager=0) */
	if (!dd || !dd->mem || !dd->mem_phys)
		return -ENODEV;
	if (dd->mem_size <= SEV_GPU_DATA_HEADER_SIZE)
		return -ENOSPC;
	if (copy_from_user(&su, argp, sizeof(su)))
		return -EFAULT;
	if (su.length == 0)
		return -EINVAL;

	hdr = dd->mem;

	/* The manager initializes the header; bail if it has not yet. */
	memcpy_fromio(&magic, hdr + offsetof(sev_gpu_data_header_t, magic),
		      sizeof(magic));
	if (magic != SEV_GPU_DATA_MAGIC)
		return -ENODEV;

	mutex_lock(&d->copy_lock);

	/*
	 * Publish the request parameters (and clear the status) with the state
	 * still un-staged, then flip state to STAGED last so the manager never
	 * observes STAGED before the parameters land.
	 */
	iowrite32(su.channel_id, hdr + offsetof(sev_gpu_data_header_t, req_channel_id));
	iowrite32(su.flags,      hdr + offsetof(sev_gpu_data_header_t, req_flags));
	iowrite32(su.generation, hdr + offsetof(sev_gpu_data_header_t, req_generation));
	iowrite32(0,             hdr + offsetof(sev_gpu_data_header_t, req_status));
	memcpy_toio(hdr + offsetof(sev_gpu_data_header_t, req_src_offset),
		    &su.src_offset, sizeof(su.src_offset));
	memcpy_toio(hdr + offsetof(sev_gpu_data_header_t, req_dst_offset),
		    &su.dst_offset, sizeof(su.dst_offset));
	memcpy_toio(hdr + offsetof(sev_gpu_data_header_t, req_length),
		    &su.length, sizeof(su.length));
	memcpy_toio(hdr + offsetof(sev_gpu_data_header_t, req_auth_tag_offset),
		    &su.auth_tag_offset, sizeof(su.auth_tag_offset));
	memcpy_toio(hdr + offsetof(sev_gpu_data_header_t, req_iv_offset),
		    &su.iv_offset, sizeof(su.iv_offset));
	wmb();
	iowrite32(SEV_GPU_DATA_STAGED, hdr + offsetof(sev_gpu_data_header_t, state));

	ivshmem_ring(d, sev_gpu_manager_peer(d), IVSHMEM_VECTOR_RPC);

	/* Poll for completion (manager flips state to DONE). */
	deadline = jiffies + msecs_to_jiffies(5000);
	for (;;) {
		state = ioread32(hdr + offsetof(sev_gpu_data_header_t, state));
		if (state == SEV_GPU_DATA_DONE)
			break;
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		usleep_range(20, 50);
	}

	if (ret == 0) {
		status = (s32)ioread32(hdr + offsetof(sev_gpu_data_header_t,
						      req_status));
		/* Return the region to idle for the next job. */
		iowrite32(SEV_GPU_DATA_BOUND,
			  hdr + offsetof(sev_gpu_data_header_t, state));
		ret = status;	/* manager's rc: 0 or a negative errno */
	}

	mutex_unlock(&d->copy_lock);
	return ret;
}

/*
 * Client-driven mediated work submission (todo#7): a GPU-less client cannot ring
 * a doorbell, so it stages the channel handles into its PRIVATE data-region
 * header (req_kind = SUBMIT_WORK, state STAGED) and kicks the manager, which
 * rings the channel doorbell on the real GPU and returns the result. Mirrors
 * sev_gpu_request_copy; blocks until the manager completes (or times out).
 */
static long sev_gpu_request_submit_work(struct sev_gpu_dev *d, void __user *argp)
{
	struct sev_gpu_data_dev *dd = data_devs[0];	/* client: single region */
	void __iomem *hdr;
	sev_gpu_ioctl_submit_work_t sw;
	unsigned long deadline;
	u32 state;
	s32 status;
	u64 magic;
	long ret = 0;

	if (d->is_manager)
		return -EPERM;			/* run on a client (manager=0) */
	if (!dd || !dd->mem || !dd->mem_phys)
		return -ENODEV;
	if (dd->mem_size <= SEV_GPU_DATA_HEADER_SIZE)
		return -ENOSPC;
	if (copy_from_user(&sw, argp, sizeof(sw)))
		return -EFAULT;
	if (!sw.h_client || !sw.h_channel)
		return -EINVAL;

	hdr = dd->mem;

	/* The manager initializes the header; bail if it has not yet. */
	memcpy_fromio(&magic, hdr + offsetof(sev_gpu_data_header_t, magic),
		      sizeof(magic));
	if (magic != SEV_GPU_DATA_MAGIC)
		return -ENODEV;

	mutex_lock(&d->copy_lock);

	/*
	 * Publish the request parameters (and clear the status) with the state
	 * still un-staged, then flip state to STAGED last so the manager never
	 * observes STAGED before the parameters land.
	 */
	iowrite32(SEV_GPU_REQ_KIND_SUBMIT_WORK,
		  hdr + offsetof(sev_gpu_data_header_t, req_kind));
	iowrite32(sw.h_client,  hdr + offsetof(sev_gpu_data_header_t, req_h_client));
	iowrite32(sw.h_channel, hdr + offsetof(sev_gpu_data_header_t, req_h_channel));
	iowrite32(0,            hdr + offsetof(sev_gpu_data_header_t, req_status));
	wmb();
	iowrite32(SEV_GPU_DATA_STAGED, hdr + offsetof(sev_gpu_data_header_t, state));

	ivshmem_ring(d, sev_gpu_manager_peer(d), IVSHMEM_VECTOR_RPC);

	/* Poll for completion (manager flips state to DONE). */
	deadline = jiffies + msecs_to_jiffies(5000);
	for (;;) {
		state = ioread32(hdr + offsetof(sev_gpu_data_header_t, state));
		if (state == SEV_GPU_DATA_DONE)
			break;
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		usleep_range(20, 50);
	}

	if (ret == 0) {
		status = (s32)ioread32(hdr + offsetof(sev_gpu_data_header_t,
						      req_status));
		/* Return the region to idle for the next job. */
		iowrite32(SEV_GPU_DATA_BOUND,
			  hdr + offsetof(sev_gpu_data_header_t, state));
		ret = status;	/* manager's rc: 0 or a negative errno */
	}

	mutex_unlock(&d->copy_lock);
	return ret;
}

/*
 * Client: ring the GPU doorbells for every compute channel the manager has
 * assigned to this VM. No userspace argument -- the kernel iterates its own
 * assignment state on the manager side. Mirrors sev_gpu_request_submit_work
 * but uses req_kind=FLUSH_ALL; the manager services it by iterating its
 * assign_state table and calling submit_work_fn for every in-use COMPUTE entry.
 */
static long sev_gpu_flush_channels(struct sev_gpu_dev *d)
{
	struct sev_gpu_data_dev *dd = data_devs[0];	/* client: single region */
	void __iomem *hdr;
	unsigned long deadline;
	u32 state;
	s32 status;
	u64 magic;
	long ret = 0;

	if (d->is_manager)
		return -EPERM;
	if (!dd || !dd->mem || !dd->mem_phys)
		return -ENODEV;
	if (dd->mem_size <= SEV_GPU_DATA_HEADER_SIZE)
		return -ENOSPC;

	hdr = dd->mem;
	memcpy_fromio(&magic, hdr + offsetof(sev_gpu_data_header_t, magic),
		      sizeof(magic));
	if (magic != SEV_GPU_DATA_MAGIC)
		return -ENODEV;

	mutex_lock(&d->copy_lock);

	iowrite32(SEV_GPU_REQ_KIND_FLUSH_ALL,
		  hdr + offsetof(sev_gpu_data_header_t, req_kind));
	iowrite32(0, hdr + offsetof(sev_gpu_data_header_t, req_status));
	wmb();
	iowrite32(SEV_GPU_DATA_STAGED, hdr + offsetof(sev_gpu_data_header_t, state));

	ivshmem_ring(d, sev_gpu_manager_peer(d), IVSHMEM_VECTOR_RPC);

	deadline = jiffies + msecs_to_jiffies(5000);
	for (;;) {
		state = ioread32(hdr + offsetof(sev_gpu_data_header_t, state));
		if (state == SEV_GPU_DATA_DONE)
			break;
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		usleep_range(20, 50);
	}

	if (ret == 0) {
		status = (s32)ioread32(hdr + offsetof(sev_gpu_data_header_t,
						      req_status));
		iowrite32(SEV_GPU_DATA_BOUND,
			  hdr + offsetof(sev_gpu_data_header_t, state));
		ret = status;
	}

	mutex_unlock(&d->copy_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Phase D4: in-kernel sealed KMB exchange                             */
/* ------------------------------------------------------------------ */

/*
 * The sealed-KMB mailbox lives in the SHARED CONTROL BAR, one fixed-stride slot
 * per client VM, in the free gap between the TLS tunnel and the RM-RPC region.
 * Returns NULL if the control device isn't bound or its BAR is too small.
 */
void __iomem *kmb_mailbox(u8 vm)
{
	size_t off;

	if (!ctrl_dev || !ctrl_dev->shmem || vm >= SEV_GPU_MAX_VMS)
		return NULL;
	off = SEV_GPU_KMB_REGION_OFF + (size_t)vm * SEV_GPU_KMB_SLOT_STRIDE;
	if (off + sizeof(struct sev_gpu_kmb_mbox) > ctrl_dev->shmem_size)
		return NULL;
	return ctrl_dev->shmem + off;
}

/*
 * AES-256-GCM seal (enc=true) / unseal (enc=false) of @data in place, keyed by
 * the per-client comm key, authenticated over @aad. On encrypt, @tag receives
 * the 16-byte GCM tag; on decrypt, @tag supplies it and a mismatch fails the
 * call (-EBADMSG). The plaintext KMB never leaves kernel memory.
 */
/*
 * Crypto primitives (aead, kmb_fp, get_psk, sha256, hmac_sha256,
 * hkdf_expand32, hs_derive, ecdhe_*) and struct sev_gpu_ecdhe moved to
 * sev_gpu_crypto.{h,c}. Transport-agnostic, role-agnostic.
 */



/*
 * Manager: seal the assigned channel's KMB under the comm key, post it to the
 * client's KMB mailbox, and block until the client acks (up to to_ms, 0 =
 * default 120s). fp_out[8] receives SHA256[:8] of the plaintext KMB on success.
 */

/* ===================================================================== *
 *  In-kernel mTLS-equivalent handshake (auto_mtls)                       *
 *                                                                        *
 *  Establishes the manager<->client AES-256 comm key entirely in-kernel  *
 *  via an authenticated ephemeral ECDHE-PSK agreement over the ivshmem   *
 *  handshake mailbox -- no userspace keybroker is spawned. This is the   *
 *  TLS 1.3 "psk_dhe_ke" construction reduced to primitives:              *
 *    - ephemeral NIST P-256 ECDH  -> forward secrecy                      *
 *    - shared 32-byte PSK folded into HKDF-Extract -> mutual auth         *
 *    - explicit HMAC-SHA256 Finished messages -> MITM/tamper detection    *
 *  Because ivshmem is host-visible and host-writable under SEV-SNP, an    *
 *  unauthenticated ECDH would be silently MITM-able; the PSK closes that. *
 * ===================================================================== */


/* Mailbox state word. Distinct 32-bit tags so a zeroed/garbage BAR never
 * looks like a live request. */

/* Client -> manager message types. */


/* Per-VM handshake mailbox, laid out at the start of each VM's slot in the
 * (retired keybroker) TLS ring region. Written/read with memcpy_*io. */
/* sev_gpu_hs_slot_t + HS constants -> sev_gpu_handshake.h */

/* Manager per-VM state carried between HELLO and FINISHED. Accessed only from
 * the single manager poller kthread, so no lock is needed here; the comm key
 * commit itself takes comm_keystore.lock. */
struct sev_gpu_hs_mgr_state hs_mgr_state[SEV_GPU_MAX_VMS];

/* Client-side per-VM run guards (multiple CUDA threads may forward at once). */
atomic_t hs_client_busy[SEV_GPU_MAX_VMS];
atomic_t hs_client_attempts[SEV_GPU_MAX_VMS];

/* Cached shared PSK (loaded lazily from psk_path on first handshake). */
static u8 sev_gpu_psk[SEV_GPU_COMM_KEY_LEN];
static bool sev_gpu_psk_valid;
static DEFINE_MUTEX(sev_gpu_psk_lock);

int sev_gpu_get_psk(u8 out[SEV_GPU_COMM_KEY_LEN])
{
	struct file *f;
	loff_t pos = 0;
	int n, ret = 0;

	mutex_lock(&sev_gpu_psk_lock);
	if (sev_gpu_psk_valid) {
		memcpy(out, sev_gpu_psk, SEV_GPU_COMM_KEY_LEN);
		goto out;
	}
	if (!psk_path || !*psk_path) {
		ret = -ENOENT;
		goto out;
	}
	f = filp_open(psk_path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		pr_warn("sev_gpu: auto-mTLS: cannot open PSK %s (%d)\n",
			psk_path, ret);
		goto out;
	}
	n = kernel_read(f, sev_gpu_psk, SEV_GPU_COMM_KEY_LEN, &pos);
	filp_close(f, NULL);
	if (n != SEV_GPU_COMM_KEY_LEN) {
		pr_warn("sev_gpu: auto-mTLS: PSK %s short read %d (need %d)\n",
			psk_path, n, SEV_GPU_COMM_KEY_LEN);
		memzero_explicit(sev_gpu_psk, sizeof(sev_gpu_psk));
		ret = -EINVAL;
		goto out;
	}
	sev_gpu_psk_valid = true;
	memcpy(out, sev_gpu_psk, SEV_GPU_COMM_KEY_LEN);
	pr_info("sev_gpu: auto-mTLS: loaded shared PSK from %s\n", psk_path);
out:
	mutex_unlock(&sev_gpu_psk_lock);
	return ret;
}

/* Handshake mailbox for VM @vm: start of its slot in the TLS ring region. */
void __iomem *hs_ctrl_mailbox(u8 vm)
{
	size_t off;

	if (!ctrl_dev || !ctrl_dev->shmem || vm >= SEV_GPU_MAX_VMS)
		return NULL;
	off = SEV_GPU_TLS_REGION_OFF + (size_t)vm * SEV_GPU_TLS_SLOT_STRIDE;
	if (off + sizeof(sev_gpu_hs_slot_t) > ctrl_dev->shmem_size)
		return NULL;
	return ctrl_dev->shmem + off;
}

/* Load (and cache) the 32-byte shared PSK from psk_path. */







/*
 * On-demand sealed CC_KMB pull (GET_KMB), the confidential counterpart to the
 * mailbox handshake above. When a client's CUDA issues NVC56F_CTRL_CMD_GET_KMB,
 * nvidia.ko does NOT forward the raw control (the KMB is channel key material and
 * the RM-RPC staging window lives in host-visible ivshmem). Instead the manager
 * fetches the KMB from the real GPU and calls sev_gpu_kmb_seal_impl to seal it
 * under the client's comm key; the client calls sev_gpu_kmb_install_impl to
 * unseal + install it. These are registered into nvidia.ko as the seal/install
 * hooks. Return value is an NV_STATUS surfaced to CUDA (0 == NV_OK).
 */

atomic_t sev_gpu_kmb_pull_seq = ATOMIC_INIT(0);


long sev_gpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct sev_gpu_dev *d = filp->private_data;
	void __user *argp = (void __user *)arg;

	if (!d)
		return -ENODEV;

	switch (cmd) {
	case SEV_GPU_IOC_REGISTER_VM: {
		sev_gpu_ioctl_register_vm_t reg;

		if (copy_from_user(&reg, argp, sizeof(reg)))
			return -EFAULT;

		if (d->is_manager) {
			register_vm(&reg);
		} else {
			/* Client: adopt ivposition as our slot when valid. */
			d->client_vm_id =
				(d->ivposition > 0 &&
				 d->ivposition < SEV_GPU_MAX_VMS) ?
					(u8)d->ivposition : reg.vm_id;
			/* Ensure we have the layout before any request. */
			if (!d->request_off)
				client_read_layout(d);
		}
		break;
	}

	case SEV_GPU_IOC_GET_SHMEM: {
		sev_gpu_ioctl_get_shmem_t si;

		si.phys_addr = d->shmem_phys;
		si.size      = d->shmem_size;
		if (copy_to_user(argp, &si, sizeof(si)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_GET_ROLE: {
		sev_gpu_ioctl_get_role_t ro;

		ro.is_manager = d->is_manager ? 1 : 0;
		ro.ivposition = d->ivposition;
		ro.num_vms    = manager_state.num_vms;
		if (copy_to_user(argp, &ro, sizeof(ro)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_REQUEST_GPU: {
		sev_gpu_ioctl_request_gpu_t rq;
		gpu_request_t r;
		gpu_grant_t z;
		u8 vm;

		if (copy_from_user(&rq, argp, sizeof(rq)))
			return -EFAULT;
		if (!d->request_off && client_read_layout(d))
			return -EAGAIN;	/* manager hasn't initialized yet */

		vm = d->is_manager ? rq.vm_id : d->client_vm_id;
		if (vm >= SEV_GPU_MAX_VMS)
			return -EINVAL;

		/* Clear any stale grant for this slot. */
		memset(&z, 0, sizeof(z));
		memcpy_toio(grant_slot(d, vm), &z, sizeof(z));

		memset(&r, 0, sizeof(r));
		r.vm_id        = vm;
		r.priority     = rq.priority;
		r.msg_type     = GPU_REQ_TIME;
		r.duration_us  = rq.duration_us;
		r.timestamp_ns = ktime_get_real_ns();
		memcpy_toio(req_slot(d, vm), &r, sizeof(r));

		if (d->is_manager) {
			if (d->sched_wq)
				queue_work(d->sched_wq, &d->sched_work);
		} else {
			ivshmem_ring(d, sev_gpu_manager_peer(d),
				     IVSHMEM_VECTOR_NEW_REQUEST);
		}
		pr_info("sev_gpu: VM%d requested GPU for %u us (prio %u)\n",
			vm, rq.duration_us, rq.priority);
		break;
	}

	case SEV_GPU_IOC_WAIT_GRANT: {
		sev_gpu_ioctl_wait_grant_t wg;
		gpu_grant_t g;
		long ret;
		u8 vm;

		if (copy_from_user(&wg, argp, sizeof(wg)))
			return -EFAULT;

		vm = d->is_manager ? wg.vm_id : d->client_vm_id;
		if (vm >= SEV_GPU_MAX_VMS)
			return -EINVAL;

		if (!d->is_manager && d->nvectors) {
			/*
			 * Client hybrid path: block until the FIRST grant
			 * interrupt (or the grant is already visible), then
			 * switch to short polling of the grant slot instead of
			 * taking an interrupt per update. The IRQ handler masks
			 * GRANT_READY on that first interrupt; cli_irq_rearm()
			 * re-enables it on every exit below.
			 */
			bool infinite = (wg.timeout_ms < 0);
			unsigned long deadline = jiffies +
				(infinite ? 0 : msecs_to_jiffies(wg.timeout_ms));

			/* Phase 1: wait for the first notification. */
			if (infinite) {
				ret = wait_event_interruptible(d->grant_wq,
					grant_ready(d, vm) ||
					atomic_read(&d->cli_polling));
				if (ret) {
					cli_irq_rearm(d);
					return -EINTR;
				}
			} else {
				ret = wait_event_interruptible_timeout(
					d->grant_wq,
					grant_ready(d, vm) ||
					atomic_read(&d->cli_polling),
					msecs_to_jiffies(wg.timeout_ms));
				if (ret == 0) {
					cli_irq_rearm(d);
					return -ETIMEDOUT;
				}
				if (ret < 0) {
					cli_irq_rearm(d);
					return -EINTR;
				}
			}

			/* Phase 2: poll the grant slot until it's ready. */
			while (!grant_ready(d, vm)) {
				if (!infinite && time_after(jiffies, deadline)) {
					cli_irq_rearm(d);
					return -ETIMEDOUT;
				}
				if (signal_pending(current)) {
					cli_irq_rearm(d);
					return -EINTR;
				}
				usleep_range(20, 50);
			}
			cli_irq_rearm(d);
		} else if (wg.timeout_ms < 0) {
			ret = wait_event_interruptible(d->grant_wq,
						       grant_ready(d, vm));
			if (ret)
				return -EINTR;
		} else {
			ret = wait_event_interruptible_timeout(
				d->grant_wq, grant_ready(d, vm),
				msecs_to_jiffies(wg.timeout_ms));
			if (ret == 0)
				return -ETIMEDOUT;
			if (ret < 0)
				return -EINTR;
		}

		memcpy_fromio(&g, grant_slot(d, vm), sizeof(g));
		wg.grant = g;
		if (copy_to_user(argp, &wg, sizeof(wg)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_RELEASE_GPU: {
		sev_gpu_ioctl_release_gpu_t rel;
		gpu_grant_t z;
		u8 vm;

		if (copy_from_user(&rel, argp, sizeof(rel)))
			return -EFAULT;
		if (!d->grant_off && client_read_layout(d))
			return -EAGAIN;

		vm = d->is_manager ? rel.vm_id : d->client_vm_id;
		if (vm >= SEV_GPU_MAX_VMS)
			return -EINVAL;

		memset(&z, 0, sizeof(z));
		memcpy_toio(grant_slot(d, vm), &z, sizeof(z));

		spin_lock(&manager_state.lock);
		if (manager_state.gpu_owner == vm)
			manager_state.gpu_owner = 0xFF;
		spin_unlock(&manager_state.lock);

		if (d->is_manager) {
			if (d->sched_wq)
				queue_work(d->sched_wq, &d->sched_work);
		} else {
			ivshmem_ring(d, sev_gpu_manager_peer(d),
				     IVSHMEM_VECTOR_RELEASE);
		}
		pr_info("sev_gpu: VM%d released GPU\n", vm);
		break;
	}

	case SEV_GPU_IOC_RPC_TEST:
		return sev_gpu_rpc_client_call(d, argp);

	case SEV_GPU_IOC_SET_COMM_KEY: {
		sev_gpu_ioctl_set_comm_key_t ck;
		u8 vm;

		if (copy_from_user(&ck, argp, sizeof(ck)))
			return -EFAULT;
		if (ck.key_len != SEV_GPU_COMM_KEY_LEN) {
			memzero_explicit(&ck, sizeof(ck));
			return -EINVAL;
		}

		/*
		 * Both manager and client now key by the keybroker's logical
		 * vm_id (ck.vm_id). On the client this overrides the
		 * ivposition-derived default so its slot matches the one the
		 * manager addresses.
		 */
		vm = ck.vm_id;
		if (vm >= SEV_GPU_MAX_VMS) {
			memzero_explicit(&ck, sizeof(ck));
			return -EINVAL;
		}

		/*
		 * The keybroker handshake defines the client's LOGICAL id: the
		 * manager addresses this client by vm_id, so the client must use
		 * the same vm_id for its comm key + KMB mailbox slot. This is
		 * distinct from client_vm_id (the ivposition-derived index used by
		 * the legacy request/grant path).
		 */
		if (!d->is_manager)
			d->comm_vm_id = vm;

		spin_lock(&comm_keystore.lock);
		memcpy(comm_keystore.key[vm], ck.key, SEV_GPU_COMM_KEY_LEN);
		set_bit(vm, &comm_keystore.valid);
		spin_unlock(&comm_keystore.lock);

		/* Scrub the userspace copy that transited the kernel stack. */
		memzero_explicit(&ck, sizeof(ck));

		pr_info("sev_gpu: stored manager<->client comm key for VM%d (%s)\n",
			vm, d->is_manager ? "manager" : "client");
		/*
		 * Auto-handshake (Phase D4): the in-kernel KMB exchange sealed
		 * with this comm key is normally driven by hand (ASSIGN + SEND on
		 * the manager, RECV on the client). When auto_handshake is set we
		 * kick a workqueue to run that exchange automatically: the manager
		 * pulls the real CC_KMB from nvidia.ko, AEAD-seals it under
		 * comm_keystore.key[vm] onto the tunnel, and the client unseals +
		 * installs it into its channel -- no userspace steps required.
		 */
		if (auto_handshake && hs_state.wq) {
			if (d->is_manager) {
				spin_lock(&hs_state.lock);
				set_bit(vm, &hs_state.pending);
				spin_unlock(&hs_state.lock);
			}
			queue_work(hs_state.wq, &hs_state.work);
		}
		break;
	}

	case SEV_GPU_IOC_ASSIGN_CHANNEL: {
		sev_gpu_ioctl_assign_t as;
		u32 channel_id = 0, h_client = 0, h_channel = 0;
		int ret;

		if (!d->is_manager)
			return -EPERM;	/* manager owns + assigns all channels */
		if (copy_from_user(&as, argp, sizeof(as)))
			return -EFAULT;

		ret = sev_gpu_assign_channel(as.vm_id, as.keyspace,
					     as.h_client, as.h_channel,
					     as.channel_id, &channel_id,
					     &h_client, &h_channel);
		if (ret)
			return ret;

		/* Report the manager's choice back to the caller. */
		as.channel_id = channel_id;
		as.h_client   = h_client;
		as.h_channel  = h_channel;
		if (copy_to_user(argp, &as, sizeof(as)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_ASSIGN_COMPUTE: {
		sev_gpu_ioctl_assign_compute_t ac;
		u32 channel_id = 0, h_client = 0, h_channel = 0;
		u64 userd_off = 0, gpfifo_off = 0, pushbuf_off = 0, pushbuf_gpu_va = 0;
		int ret;

		if (!d->is_manager)
			return -EPERM;	/* manager owns + assigns all channels */
		if (copy_from_user(&ac, argp, sizeof(ac)))
			return -EFAULT;

		ret = sev_gpu_assign_compute_channel(ac.vm_id, ac.flags,
						     &channel_id, &h_client,
						     &h_channel, &userd_off,
						     &gpfifo_off, &pushbuf_off,
						     &pushbuf_gpu_va);
		if (ret)
			return ret;

		ac.channel_id = channel_id;
		ac.h_client   = h_client;
		ac.h_channel  = h_channel;
		ac.userd_off  = userd_off;
		ac.gpfifo_off = gpfifo_off;
		ac.pushbuf_off = pushbuf_off;
		ac.pushbuf_gpu_va = pushbuf_gpu_va;
		if (copy_to_user(argp, &ac, sizeof(ac)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_PROVISION_POOL: {
		sev_gpu_ioctl_provision_t pv;
		sev_gpu_chan_alloc_t alloc;
		u32 done = 0, n;
		int i;

		if (!d->is_manager)
			return -EPERM;	/* manager is the sole channel allocator */
		if (copy_from_user(&pv, argp, sizeof(pv)))
			return -EFAULT;
		if (pv.count == 0)
			return -EINVAL;
		/* The registry holds at most SEV_GPU_CC_POOL_MAX; don't burn GPU
		 * allocations we can't record. */
		if (pv.count > SEV_GPU_CC_POOL_MAX)
			pv.count = SEV_GPU_CC_POOL_MAX;

		alloc = READ_ONCE(chan_alloc_fn);
		if (!alloc)
			return -ENODEV;	/* no GPU provisioner bound */

		for (n = 0; n < pv.count; n++) {
			u32 hc = 0, hch = 0, st;

			st = alloc(pv.keyspace, pv.flags, &hc, &hch);
			if (st != 0) {	/* 0 == NV_OK */
				pr_warn("sev_gpu: CC-channel alloc failed (keyspace %u) status 0x%x\n",
					pv.keyspace, st);
				break;
			}

			spin_lock(&cc_pool.lock);
			for (i = 0; i < SEV_GPU_CC_POOL_MAX; i++) {
				if (!cc_pool.e[i].provisioned) {
					cc_pool.e[i].provisioned = true;
					cc_pool.e[i].in_use      = false;
					cc_pool.e[i].h_client    = hc;
					cc_pool.e[i].h_channel   = hch;
					cc_pool.e[i].keyspace    = pv.keyspace;
					cc_pool.e[i].channel_id  = hch;
					break;
				}
			}
			spin_unlock(&cc_pool.lock);

			if (i == SEV_GPU_CC_POOL_MAX) {
				/* registry full: hand the channel back, stop */
				if (chan_free_fn)
					chan_free_fn(hc);
				break;
			}
			done++;
		}

		pv.provisioned = done;
		if (copy_to_user(argp, &pv, sizeof(pv)))
			return -EFAULT;
		pr_info("sev_gpu: provisioned %u/%u CC channel(s) keyspace %u\n",
			done, pv.count, pv.keyspace);
		if (done == 0)
			return -EIO;
		break;
	}

	case SEV_GPU_IOC_SUBMIT_COPY: {
		sev_gpu_ioctl_submit_t su;
		int rc;

		if (!d->is_manager)
			return -EPERM;	/* manager is the sole doorbell-ringer */
		if (copy_from_user(&su, argp, sizeof(su)))
			return -EFAULT;

		rc = sev_gpu_do_ce_copy(su.vm_id, su.channel_id, su.flags,
					su.generation, su.src_offset,
					su.dst_offset, su.length,
					su.auth_tag_offset, su.iv_offset);
		if (rc)
			return rc;
		break;
	}

	case SEV_GPU_IOC_REQUEST_COPY:
		return sev_gpu_request_copy(d, argp);

	case SEV_GPU_IOC_SUBMIT_WORK: {
		sev_gpu_ioctl_submit_work_t sw;
		int rc;

		if (!d->is_manager)
			return -EPERM;	/* manager is the sole doorbell-ringer */
		if (copy_from_user(&sw, argp, sizeof(sw)))
			return -EFAULT;
		if (sw.vm_id >= SEV_GPU_MAX_VMS)
			return -EINVAL;

		rc = sev_gpu_do_submit_work(sw.vm_id, sw.h_client,
					    sw.h_channel, false);
		if (rc)
			return rc;
		break;
	}

	case SEV_GPU_IOC_REQUEST_SUBMIT:
		return sev_gpu_request_submit_work(d, argp);

	case SEV_GPU_IOC_GET_COMPUTE_INFO: {
		sev_gpu_ioctl_compute_info_t ci;
		struct sev_gpu_data_dev *dd = data_devs[0];  /* client: single region */
		u64 userd_gpa = 0, gpfifo_gpa = 0, pushbuf_gpa = 0, enc_gpa = 0;
		u64 userd_off = 0, gpfifo_off = 0, pushbuf_off = 0, enc_off = 0;
		int ret;

		if (d->is_manager)
			return -EPERM;	/* client-only geometry query */
		if (!dd || !dd->mem_phys)
			return -ENODEV;
		if (copy_from_user(&ci, argp, sizeof(ci)))
			return -EFAULT;

		ret = sev_gpu_compute_carve(dd, ci.chan_idx,
					    &userd_gpa, &gpfifo_gpa, &pushbuf_gpa,
					    &userd_off, &gpfifo_off, &pushbuf_off,
					    &enc_gpa, &enc_off);
		if (ret)
			return ret;

		ci.userd_off   = userd_off;
		ci.gpfifo_off  = gpfifo_off;
		ci.pushbuf_off = pushbuf_off;
		ci.enc_off     = enc_off;
		if (copy_to_user(argp, &ci, sizeof(ci)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_FLUSH_CHANNELS:
		return sev_gpu_flush_channels(d);

	case SEV_GPU_IOC_SEND_KMB: {
		sev_gpu_ioctl_send_kmb_t sk;
		u8 fp[8];
		int ret;

		if (!d->is_manager)
			return -EPERM;
		if (copy_from_user(&sk, argp, sizeof(sk)))
			return -EFAULT;

		ret = sev_gpu_send_kmb(sk.vm_id, sk.channel_id, sk.timeout_ms,
				       fp);
		if (ret)
			return ret;

		memcpy(sk.fp, fp, sizeof(sk.fp));
		if (copy_to_user(argp, &sk, sizeof(sk)))
			return -EFAULT;
		pr_info("sev_gpu: sent KMB ch %u to VM%u fp %02x%02x%02x%02x%02x%02x%02x%02x\n",
			sk.channel_id, sk.vm_id, fp[0], fp[1], fp[2], fp[3],
			fp[4], fp[5], fp[6], fp[7]);
		break;
	}

	case SEV_GPU_IOC_RECV_KMB: {
		sev_gpu_ioctl_recv_kmb_t rk;
		u32 channel_id = 0, keyspace = 0;
		u8 fp[8];
		int ret;

		if (d->is_manager)
			return -EPERM;	/* client-only */
		if (copy_from_user(&rk, argp, sizeof(rk)))
			return -EFAULT;

		ret = sev_gpu_recv_kmb(d, rk.timeout_ms, &channel_id, &keyspace,
				       fp);
		if (ret)
			return ret;

		rk.channel_id = channel_id;
		rk.keyspace   = keyspace;
		memcpy(rk.fp, fp, sizeof(rk.fp));
		if (copy_to_user(argp, &rk, sizeof(rk)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_CRYPT: {
		sev_gpu_ioctl_crypt_t cr;
		struct sev_cc_kmb kmb;
		const struct sev_cc_aes_cryptobundle *b;
		u8 key[SEV_GPU_COMM_KEY_LEN];
		u8 gcm_iv[SEV_GPU_KMB_NONCE_LEN];
		u8 counter[SEV_GPU_KMB_NONCE_LEN];
		u8 tag[SEV_GPU_KMB_TAG_LEN];
		void *buf;
		u64 ctr = 0;
		u32 generation = 0;
		bool enc, use_dec_bundle, found = false;
		int i, ret = 0;

		if (d->is_manager)
			return -EPERM;	/* client-only data plane */
		if (copy_from_user(&cr, argp, sizeof(cr)))
			return -EFAULT;
		if (cr.length == 0 || cr.length > SEV_GPU_CRYPT_MAX)
			return -EINVAL;

		enc            = !(cr.flags & SEV_GPU_CRYPT_F_DECRYPT);
		use_dec_bundle = cr.flags & SEV_GPU_CRYPT_F_USE_DECRYPT_BUNDLE;

		/* Find the installed KMB; reserve a counter on encrypt. */
		spin_lock(&client_kmb_store.lock);
		for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
			struct sev_client_chan *e = &client_kmb_store.c[i];

			if (e->valid && e->channel_id == cr.channel_id) {
				u64 *pctr = use_dec_bundle ?
					&e->ctr_d2h : &e->ctr_h2d;

				/*
				 * D4.3d: never let the IV counter reach the
				 * rotation threshold -- that would risk reusing
				 * a (key, IV) pair. Refuse and force a rotation.
				 */
				if (enc && *pctr + 1 >= SEV_GPU_IV_ROTATE_THRESHOLD) {
					spin_unlock(&client_kmb_store.lock);
					return -EOVERFLOW;
				}
				kmb = e->kmb;
				generation = e->generation;
				if (enc)
					ctr = ++(*pctr);
				found = true;
				break;
			}
		}
		spin_unlock(&client_kmb_store.lock);
		if (!found)
			return -ENOENT;	/* no KMB installed for this channel */

		/*
		 * D4.3d: on decrypt, reject ciphertext made under a different KMB
		 * epoch than the one currently installed (the channel rotated).
		 */
		if (!enc && cr.generation != 0 && cr.generation != generation)
			return -ESTALE;

		b = use_dec_bundle ? &kmb.u.decryptBundle : &kmb.encryptBundle;
		memcpy(key, b->key, sizeof(key));

		/*
		 * 96-bit message counter: ours (incrementing) on encrypt, the
		 * caller's (GPU-produced) on decrypt. The real GCM IV is the
		 * counter XOR ivMask, matching the CC CE on the GPU side.
		 */
		memset(counter, 0, sizeof(counter));
		if (enc) {
			for (i = 0; i < 8; i++)
				counter[i] = (u8)(ctr >> (8 * i));
		} else {
			memcpy(counter, cr.iv, sizeof(counter));
		}
		for (i = 0; i < (int)sizeof(gcm_iv); i++)
			gcm_iv[i] = counter[i] ^ ((const u8 *)b->ivMask)[i];

		buf = kmalloc(cr.length, GFP_KERNEL);
		if (!buf) {
			memzero_explicit(key, sizeof(key));
			memzero_explicit(&kmb, sizeof(kmb));
			return -ENOMEM;
		}
		if (copy_from_user(buf, (void __user *)(uintptr_t)cr.data,
				   cr.length)) {
			ret = -EFAULT;
			goto crypt_out;
		}
		if (!enc)
			memcpy(tag, cr.tag, sizeof(tag));

		ret = sev_gpu_aead(enc, key, gcm_iv, NULL, 0,
				   buf, cr.length, tag);
		if (ret)
			goto crypt_out;	/* -EBADMSG on auth failure */

		if (copy_to_user((void __user *)(uintptr_t)cr.data, buf,
				 cr.length)) {
			ret = -EFAULT;
			goto crypt_out;
		}
		if (enc) {
			memcpy(cr.iv, counter, sizeof(cr.iv));
			memcpy(cr.tag, tag, sizeof(cr.tag));
			cr.generation = generation;	/* epoch pin for the manager */
			if (copy_to_user(argp, &cr, sizeof(cr)))
				ret = -EFAULT;
		}

crypt_out:
		memzero_explicit(buf, cr.length);
		kfree(buf);
		memzero_explicit(key, sizeof(key));
		memzero_explicit(&kmb, sizeof(kmb));
		if (ret)
			return ret;
		break;
	}

	case SEV_GPU_IOC_CHAN_STATUS: {
		sev_gpu_ioctl_chan_status_t cs;
		int i;

		if (d->is_manager)
			return -EPERM;	/* client-only data plane */
		if (copy_from_user(&cs, argp, sizeof(cs)))
			return -EFAULT;

		cs.valid      = 0;
		cs.generation = 0;
		cs.keyspace   = 0;
		cs.ctr_h2d    = 0;
		cs.ctr_d2h    = 0;

		spin_lock(&client_kmb_store.lock);
		for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
			struct sev_client_chan *e = &client_kmb_store.c[i];

			if (e->valid && e->channel_id == cs.channel_id) {
				cs.valid      = 1;
				cs.generation = e->generation;
				cs.keyspace   = e->keyspace;
				cs.ctr_h2d    = e->ctr_h2d;
				cs.ctr_d2h    = e->ctr_d2h;
				break;
			}
		}
		spin_unlock(&client_kmb_store.lock);

		if (copy_to_user(argp, &cs, sizeof(cs)))
			return -EFAULT;
		break;
	}

	default:
		return -ENOTTY;
	}
	return 0;
}




/* ------------------------------------------------------------------ */
/* Private per-VM data device (/dev/sev_gpu_dataN)                     */
/* ------------------------------------------------------------------ */









/* ------------------------------------------------------------------ */
/* RM-RPC forwarding (client) + replay (manager)                       */
/* ------------------------------------------------------------------ */

/*
 * Lazy synthetic-GPU attach (defined later): retried from the client forward
 * path so /dev/nvidia0 materializes once the manager has published its GPU
 * identity, even if that happens after this client bound its control device.
 */
void sev_gpu_client_attach_gpu(struct sev_gpu_dev *d);


/*
 * Manager: bind nvidia.ko's real-GPU replay handler so forwarded RM escapes
 * execute on the physical GPU. Done via symbol_get (like the client forwarder
 * bind) so this module stays loadable when nvidia.ko is absent -- the transport
 * just stays in echo mode until a replay handler is bound. Keeping every
 * cross-module symbol_get on this side means nvidia.ko never depends on
 * sev_gpu_manager.ko, so there is no circular module dependency.
 */
extern u32 sev_gpu_rm_replay(u32 client_id, u32 cmd, void *arg, u32 size);
extern void sev_gpu_rm_replay_teardown(void);





typedef void (*sev_gpu_register_shadow_db_t)(u64 (*fn)(u32 client_id));
static void (*shadow_db_unregister_fn)(void);
extern void sev_gpu_register_shadow_db(u64 (*fn)(u32 client_id));
extern void sev_gpu_unregister_shadow_db(void);

typedef void (*sev_gpu_register_osdesc_carve_t)(
	int (*fn)(u32 client_id, u64 size, u64 *mgr_phys, u64 *cli_phys));
static void (*osdesc_carve_unregister_fn)(void);
extern void sev_gpu_register_osdesc_carve(
	int (*fn)(u32 client_id, u64 size, u64 *mgr_phys, u64 *cli_phys));
extern void sev_gpu_unregister_osdesc_carve(void);

/* Manager: carve-pool reclaim hook -- nvidia.ko calls it once a client's last
 * ivshmem carve is freed so we can rewind that client's bump cursor. */
typedef void (*sev_gpu_register_osdesc_reset_t)(void (*fn)(u32 client_id));
static void (*osdesc_reset_unregister_fn)(void);
extern void sev_gpu_register_osdesc_reset(void (*fn)(u32 client_id));
extern void sev_gpu_unregister_osdesc_reset(void);

/* Manager: phys->kernel-VA translator for ivshmem-backed OS_PHYS_ADDR objects. */
typedef void (*sev_gpu_register_phys_to_va_t)(
	unsigned long (*fn)(u64 phys, u64 size));
static void (*phys_to_va_unregister_fn)(void);
extern void sev_gpu_register_phys_to_va(unsigned long (*fn)(u64 phys, u64 size));
extern void sev_gpu_unregister_phys_to_va(void);

/* Confidential CC_KMB seal (manager) / install (client) hooks in nvidia.ko. */
static void (*kmb_seal_unregister_fn)(void);
extern void sev_gpu_register_kmb_seal(sev_gpu_kmb_seal_t fn);
extern void sev_gpu_unregister_kmb_seal(void);
static void (*kmb_install_unregister_fn)(void);
extern void sev_gpu_register_kmb_install(sev_gpu_kmb_install_t fn);
extern void sev_gpu_unregister_kmb_install(void);

static void rpc_manager_bind_nvidia(void)
{
	sev_gpu_rm_replay_t replay;
	sev_gpu_register_shadow_db_t reg_db;

	replay = symbol_get(sev_gpu_rm_replay);
	if (!replay) {
		pr_info("sev_gpu: nvidia RM replay absent; manager echoes RM-RPC\n");
		return;
	}
	rpc_replay_teardown = symbol_get(sev_gpu_rm_replay_teardown);
	WRITE_ONCE(rpc_replay_fn, replay);
	pr_info("sev_gpu: bound nvidia.ko RM replay handler\n");

	reg_db = symbol_get(sev_gpu_register_shadow_db);
	if (reg_db) {
		shadow_db_unregister_fn = symbol_get(sev_gpu_unregister_shadow_db);
		reg_db(sev_gpu_shadow_db_impl);
		symbol_put(sev_gpu_register_shadow_db);
		pr_info("sev_gpu: registered shadow doorbell callback with nvidia.ko\n");
	}

	{
		sev_gpu_register_osdesc_carve_t reg_oc =
			symbol_get(sev_gpu_register_osdesc_carve);
		if (reg_oc) {
			osdesc_carve_unregister_fn =
				symbol_get(sev_gpu_unregister_osdesc_carve);
			reg_oc(sev_gpu_osdesc_carve_impl);
			symbol_put(sev_gpu_register_osdesc_carve);
			pr_info("sev_gpu: registered osdesc carve callback with nvidia.ko\n");
		}
	}

	{
		sev_gpu_register_osdesc_reset_t reg_or =
			symbol_get(sev_gpu_register_osdesc_reset);
		if (reg_or) {
			osdesc_reset_unregister_fn =
				symbol_get(sev_gpu_unregister_osdesc_reset);
			reg_or(sev_gpu_osdesc_reset_impl);
			symbol_put(sev_gpu_register_osdesc_reset);
			pr_info("sev_gpu: registered osdesc reset callback with nvidia.ko\n");
		}
	}

	{
		sev_gpu_register_phys_to_va_t reg_pv =
			symbol_get(sev_gpu_register_phys_to_va);
		if (reg_pv) {
			phys_to_va_unregister_fn =
				symbol_get(sev_gpu_unregister_phys_to_va);
			reg_pv(sev_gpu_shared_phys_to_va_impl);
			symbol_put(sev_gpu_register_phys_to_va);
			pr_info("sev_gpu: registered phys->va callback with nvidia.ko\n");
		}
	}

	{
		void (*reg_seal)(sev_gpu_kmb_seal_t) =
			symbol_get(sev_gpu_register_kmb_seal);
		if (reg_seal) {
			kmb_seal_unregister_fn =
				symbol_get(sev_gpu_unregister_kmb_seal);
			reg_seal(sev_gpu_kmb_seal_impl);
			symbol_put(sev_gpu_register_kmb_seal);
			pr_info("sev_gpu: registered GET_KMB seal callback with nvidia.ko\n");
		}
	}
}

static void rpc_manager_unbind_nvidia(void)
{
	if (!READ_ONCE(rpc_replay_fn))
		return;
	WRITE_ONCE(rpc_replay_fn, NULL);
	if (rpc_replay_teardown) {
		rpc_replay_teardown();
		symbol_put(sev_gpu_rm_replay_teardown);
		rpc_replay_teardown = NULL;
	}
	symbol_put(sev_gpu_rm_replay);

	if (shadow_db_unregister_fn) {
		shadow_db_unregister_fn();
		symbol_put(sev_gpu_unregister_shadow_db);
		shadow_db_unregister_fn = NULL;
	}

	if (osdesc_carve_unregister_fn) {
		osdesc_carve_unregister_fn();
		symbol_put(sev_gpu_unregister_osdesc_carve);
		osdesc_carve_unregister_fn = NULL;
	}

	if (osdesc_reset_unregister_fn) {
		osdesc_reset_unregister_fn();
		symbol_put(sev_gpu_unregister_osdesc_reset);
		osdesc_reset_unregister_fn = NULL;
	}

	if (phys_to_va_unregister_fn) {
		phys_to_va_unregister_fn();
		symbol_put(sev_gpu_unregister_phys_to_va);
		phys_to_va_unregister_fn = NULL;
	}

	if (kmb_seal_unregister_fn) {
		kmb_seal_unregister_fn();
		symbol_put(sev_gpu_unregister_kmb_seal);
		kmb_seal_unregister_fn = NULL;
	}
}

/*
 * Manager: bind nvidia.ko's in-kernel CC_KMB fetch (D4.2). Like the replay
 * bind, this is a symbol_get on this side so nvidia.ko never depends on us.
 * When absent (no GPU / nvidia.ko not loaded), assignments fall back to the
 * D4.1 placeholder KMB so the seal/transport/install path stays testable.
 */
extern u32 sev_gpu_rm_get_kmb(u32 h_client, u32 h_channel,
			      void *kmb_out, u32 kmb_size);
extern u32 sev_gpu_rm_alloc_cc_channel(u32 ce_id, u32 flags,
				       u32 *h_client, u32 *h_channel);
extern u32 sev_gpu_rm_free_cc_channel(u32 h_client);
extern u32 sev_gpu_rm_ce_submit(u32 h_client, u32 flags,
				u64 src, u64 dst, u64 length,
				u64 auth_tag, u64 iv);
extern u32 sev_gpu_rm_submit_work(u32 h_client, u32 h_channel);
extern u32 sev_gpu_rm_get_work_submit_token(u32 h_client, u32 h_channel,
					    u32 *token);
extern u32 sev_gpu_rm_alloc_compute_channel(u32 flags, u64 userd_gpa,
					    u64 gpfifo_gpa, u64 pushbuf_gpa,
					    u32 *h_client, u32 *h_channel,
					    u64 *pushbuf_gpu_va);
extern u32 sev_gpu_rm_free_compute_channel(u32 h_client);

static void kmb_manager_bind_nvidia(void)
{
	kmb_fetch_fn = symbol_get(sev_gpu_rm_get_kmb);
	if (kmb_fetch_fn)
		pr_info("sev_gpu: bound nvidia.ko CC_KMB fetch handler\n");
	else
		pr_info("sev_gpu: nvidia CC_KMB fetch absent; assignments use placeholder KMB\n");

	chan_alloc_fn = symbol_get(sev_gpu_rm_alloc_cc_channel);
	chan_free_fn  = symbol_get(sev_gpu_rm_free_cc_channel);
	if (chan_alloc_fn && chan_free_fn) {
		pr_info("sev_gpu: bound nvidia.ko CC-channel provisioner\n");
	} else {
		/* Need both alloc and free, or neither. */
		if (chan_alloc_fn) {
			symbol_put(sev_gpu_rm_alloc_cc_channel);
			chan_alloc_fn = NULL;
		}
		if (chan_free_fn) {
			symbol_put(sev_gpu_rm_free_cc_channel);
			chan_free_fn = NULL;
		}
		pr_info("sev_gpu: nvidia CC-channel provisioner absent; pool disabled\n");
	}

	/* CE secure-copy submit is optional and bound on its own (D4.3). */
	ce_submit_fn = symbol_get(sev_gpu_rm_ce_submit);
	if (ce_submit_fn)
		pr_info("sev_gpu: bound nvidia.ko CE secure-copy submit\n");
	else
		pr_info("sev_gpu: nvidia CE submit absent; mediated copies disabled\n");

	/* Compute doorbell-ring is optional and bound on its own (todo#7). */
	submit_work_fn = symbol_get(sev_gpu_rm_submit_work);
	if (submit_work_fn)
		pr_info("sev_gpu: bound nvidia.ko compute work submit\n");
	else
		pr_info("sev_gpu: nvidia work submit absent; mediated launches disabled\n");

	/* Work-submit-token query is optional (Fork B WLC launch target). */
	get_work_submit_token_fn = symbol_get(sev_gpu_rm_get_work_submit_token);
	if (get_work_submit_token_fn)
		pr_info("sev_gpu: bound nvidia.ko work-submit-token query\n");
	else
		pr_info("sev_gpu: nvidia work-submit-token query absent; WLC launch token unresolved\n");

	/* Compute (GR) channel provisioner (Arch B: manager allocates channels). */
	compute_alloc_fn = symbol_get(sev_gpu_rm_alloc_compute_channel);
	compute_free_fn  = symbol_get(sev_gpu_rm_free_compute_channel);
	if (compute_alloc_fn && compute_free_fn) {
		pr_info("sev_gpu: bound nvidia.ko compute-channel provisioner\n");
	} else {
		/* Need both alloc and free, or neither. */
		if (compute_alloc_fn) {
			symbol_put(sev_gpu_rm_alloc_compute_channel);
			compute_alloc_fn = NULL;
		}
		if (compute_free_fn) {
			symbol_put(sev_gpu_rm_free_compute_channel);
			compute_free_fn = NULL;
		}
		pr_info("sev_gpu: nvidia compute-channel provisioner absent\n");
	}
}

/*
 * L1 smoke test (Arch B bring-up): allocate, log, and immediately free
 * @compute_selftest manager-owned compute (GR) channel trees. Proves the
 * manager RM can stand up a GR engine context (client/device/subdevice +
 * VASpace + GR0 TSG) end to end. Off by default; gated by the module param.
 */
static void sev_gpu_compute_selftest(void)
{
	u32 n = compute_selftest;
	u32 done = 0;
	u64 userd_gpa = 0, gpfifo_gpa = 0, pushbuf_gpa = 0;
	u32 i;

	if (!n)
		return;
	if (!compute_alloc_fn || !compute_free_fn) {
		pr_warn("sev_gpu: compute_selftest=%u but compute provisioner not bound\n",
			n);
		return;
	}
	if (n > SEV_GPU_CC_POOL_MAX)
		n = SEV_GPU_CC_POOL_MAX;

	/*
	 * If a per-VM DATA region is already attached, exercise the zero-copy
	 * path: back the channel's USERD (page 0), GPFIFO ring (page 1), and
	 * method pushbuffer (page 2) from that shared region's payload via the
	 * OS_PHYS_ADDR descriptor. The pages are reused each iteration (the channel
	 * is freed before the next alloc). Otherwise the provisioner falls back to
	 * RM-local sysmem (0/0/0).
	 */
	if (num_data_devs > 0 && data_devs[0] && data_devs[0]->mem_phys &&
	    data_devs[0]->mem_size >= SEV_GPU_DATA_HEADER_SIZE + 3 * PAGE_SIZE) {
		u64 base = (u64)data_devs[0]->mem_phys + SEV_GPU_DATA_HEADER_SIZE;

		if (IS_ALIGNED(base, PAGE_SIZE)) {
			userd_gpa   = base;
			gpfifo_gpa  = base + PAGE_SIZE;
			pushbuf_gpa = base + 2 * PAGE_SIZE;
			pr_info("sev_gpu: compute_selftest: shared-backed USERD=0x%llx GPFIFO=0x%llx PUSH=0x%llx (DATA dev0)\n",
				userd_gpa, gpfifo_gpa, pushbuf_gpa);
		}
	}
	if (!userd_gpa)
		pr_info("sev_gpu: compute_selftest: RM-local backing (no DATA region attached)\n");

	for (i = 0; i < n; i++) {
		u32 h_client = 0, h_channel = 0;
		u64 pushbuf_gpu_va = 0;
		u32 st = compute_alloc_fn(0, userd_gpa, gpfifo_gpa, pushbuf_gpa,
					 &h_client, &h_channel, &pushbuf_gpu_va);

		if (st != 0) {
			pr_warn("sev_gpu: compute_selftest: alloc %u/%u failed status 0x%x\n",
				i + 1, n, st);
			break;
		}
		pr_info("sev_gpu: compute_selftest: alloc %u/%u hClient=0x%x hChannel=0x%x pushVA=0x%llx\n",
			i + 1, n, h_client, h_channel, pushbuf_gpu_va);

		st = compute_free_fn(h_client);
		if (st != 0)
			pr_warn("sev_gpu: compute_selftest: free hClient=0x%x failed status 0x%x\n",
				h_client, st);
		else
			done++;
	}
	pr_info("sev_gpu: compute_selftest: %u/%u channel trees alloc+freed OK\n",
		done, n);
}

/* Release every provisioned pool channel back to the GPU. */
static void cc_pool_drain(void)
{
	int i;

	if (!chan_free_fn)
		return;
	spin_lock(&cc_pool.lock);
	for (i = 0; i < SEV_GPU_CC_POOL_MAX; i++) {
		struct sev_gpu_cc_chan *e = &cc_pool.e[i];
		u32 hc;

		if (!e->provisioned)
			continue;
		hc = e->h_client;
		memset(e, 0, sizeof(*e));
		spin_unlock(&cc_pool.lock);
		chan_free_fn(hc);
		spin_lock(&cc_pool.lock);
	}
	spin_unlock(&cc_pool.lock);
}

/* Free every allocate-on-assign COMPUTE channel back to the GPU (L3.3). */
static void compute_assignments_drain(void)
{
	int vm, i;

	if (!compute_free_fn)
		return;
	spin_lock(&assign_state.lock);
	for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
		for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
			struct sev_gpu_assignment *e = &assign_state.a[vm][i];
			u32 hc;

			if (!e->in_use ||
			    e->kind != SEV_GPU_CHAN_KIND_COMPUTE)
				continue;
			hc = e->h_client;
			memset(e, 0, sizeof(*e));
			spin_unlock(&assign_state.lock);
			compute_free_fn(hc);
			spin_lock(&assign_state.lock);
		}
	}
	spin_unlock(&assign_state.lock);
}

static void kmb_manager_unbind_nvidia(void)
{
	/* Free the channel pool before dropping the allocator symbols. */
	cc_pool_drain();
	compute_assignments_drain();

	if (compute_alloc_fn) {
		symbol_put(sev_gpu_rm_alloc_compute_channel);
		compute_alloc_fn = NULL;
	}
	if (compute_free_fn) {
		symbol_put(sev_gpu_rm_free_compute_channel);
		compute_free_fn = NULL;
	}

	if (submit_work_fn) {
		symbol_put(sev_gpu_rm_submit_work);
		submit_work_fn = NULL;
	}

	if (get_work_submit_token_fn) {
		symbol_put(sev_gpu_rm_get_work_submit_token);
		get_work_submit_token_fn = NULL;
	}

	if (ce_submit_fn) {
		symbol_put(sev_gpu_rm_ce_submit);
		ce_submit_fn = NULL;
	}

	if (chan_alloc_fn) {
		symbol_put(sev_gpu_rm_alloc_cc_channel);
		chan_alloc_fn = NULL;
	}
	if (chan_free_fn) {
		symbol_put(sev_gpu_rm_free_cc_channel);
		chan_free_fn = NULL;
	}

	if (!kmb_fetch_fn)
		return;
	kmb_fetch_fn = NULL;
	symbol_put(sev_gpu_rm_get_kmb);
}



static void rpc_manager_start(void)
{
	if (rpc_kthread)
		return;
	atomic_set(&rpc_kick, 0);
	rpc_kthread = kthread_run(rpc_thread_fn, NULL, "sev_gpu_rpc");
	if (IS_ERR(rpc_kthread)) {
		pr_warn("sev_gpu: failed to start RM-RPC replay thread\n");
		rpc_kthread = NULL;
	} else {
		pr_info("sev_gpu: RM-RPC replay thread started (loopback=%d)\n",
			rpc_loopback);
	}
}

static void rpc_manager_stop(void)
{
	if (rpc_kthread) {
		kthread_stop(rpc_kthread);
		rpc_kthread = NULL;
	}
}

/*
 * Client: redirect CUDA mmap calls for channel pages (USERD / GPFIFO /
 * pushbuffer) to the ivshmem data region backing those channels.
 *
 * When CUDA on a GPU-less client calls mmap() for its channel's USERD page,
 * nvidia_mmap_helper() has no local mmap context (NV_ESC_RM_MAP_MEMORY was
 * forwarded to the manager).  nv-mmap.c calls this callback with the GPA CUDA
 * is trying to map.  We check whether the GPA falls within the compute reserve
 * of data_devs[0] (the only data device on a client).  If it does, we return
 * its PFN so nv-mmap.c can remap_pfn_range() it -- the page is already shared
 * ivshmem RAM visible to both VMs.
 */
/* mmap-redirect nvidia registration -> sev_gpu_client_mmap.c */

/* ------------------------------------------------------------------ */
/* Client: shadow USERMODE GPU-timer emulation                        */
/* ------------------------------------------------------------------ */
/*
 * The HOPPER_USERMODE_A object CUDA maps for work submission is, on a GPU-less
 * client, our static ivshmem shadow doorbell page (served above by
 * sev_gpu_mmap_redirect_impl).  Besides the doorbell at
 * NVC361_NOTIFY_CHANNEL_PENDING (0x90) that CUDA *writes*, that same page
 * exposes the GPU nanosecond PTIMER that CUDA *reads* directly during context
 * init to build its GPU<->CPU time correlation:
 *
 *   TIME_0 (0x80): ns[31:0]  -- but the low 5 bits MUST read 0.  The RM/CUDA
 *                               read loop (tmrGetTimeEx_GM107) rejects any
 *                               sample where (TIME_0 & ~DRF_SHIFTMASK(
 *                               NV_PTIMER_TIME_0_NSEC=31:5)) != 0, i.e. low 5
 *                               bits set, as an unlatched/"bad" read.
 *   TIME_1 (0x84): ns[63:32] -- only bits 28:0 significant.
 *   Reconstruction: time = ((u64)TIME_1 << 32) | TIME_0.
 *
 * On real hardware this MMIO always advances; our shadow page would read as a
 * dead 0 clock, so CUDA concludes the GPU timer is stopped and aborts
 * cudaMalloc's lazy context init with "device unavailable" (code 46).  Keep the
 * two words advancing with a client-monotonic ns value via a periodic hrtimer.
 * The value need not match the manager's real GPU PTIMER: CUDA only needs a
 * sane, advancing, self-consistent clock to correlate against its own CPU clock
 * (which is likewise client-monotonic, giving a small stable offset).
 */








/*
 * Client: optionally bind our forwarder into nvidia.ko's escape hook. Done via
 * symbol_get so this module stays loadable when nvidia.ko is absent (e.g. on an
 * ordinary VM used for transport bring-up).
 */
extern void sev_gpu_register_rm_forwarder(sev_gpu_rm_forwarder_t fn);
extern void sev_gpu_unregister_rm_forwarder(void);



/*
 * GPU identity bootstrap (control-plane enumeration).
 *
 * The manager owns the real GPU; a GPU-less client has nothing for cuInit to
 * enumerate. The manager exports its GPU's stable identity (nvidia.ko's
 * sev_gpu_export_local_gpu) and publishes it into the shared control BAR; the
 * client reads it back and materializes a synthetic /dev/nvidiaN with the same
 * gpu_id (nvidia.ko's sev_gpu_client_register_gpu), so forwarded enumeration
 * controls on the real GPU line up. Plain shared-memory descriptor, no doorbell,
 * no handshake. All three nvidia.ko entry points are bound via symbol_get so
 * this module stays loadable when nvidia.ko is absent.
 *
 * Requires nvidia.ko to be loaded before this module on the manager VM (same
 * ordering constraint as the RM replay bind).
 */
typedef u32 (*sev_gpu_export_local_gpu_t)(sev_gpu_card_desc_t *out, u32 size);
typedef u32 (*sev_gpu_client_register_gpu_t)(const sev_gpu_card_desc_t *desc,
					     u32 size);
typedef void (*sev_gpu_client_unregister_gpu_t)(void);

extern u32 sev_gpu_export_local_gpu(sev_gpu_card_desc_t *out, u32 size);
extern u32 sev_gpu_client_register_gpu(const sev_gpu_card_desc_t *desc, u32 size);
extern void sev_gpu_client_unregister_gpu(void);

static bool client_gpu_registered;	/* client: synthetic device created */

/* Manager: export the local GPU identity and publish it to the control BAR. */
static void sev_gpu_publish_gpu_desc(struct sev_gpu_dev *d)
{
	sev_gpu_export_local_gpu_t export_fn;
	sev_gpu_card_desc_t desc;
	u32 st;

	if (!d->shmem)
		return;
	if (SEV_GPU_GPUDESC_REGION_OFF + sizeof(desc) > d->shmem_size) {
		pr_warn("sev_gpu: BAR too small for GPU descriptor region\n");
		return;
	}

	export_fn = symbol_get(sev_gpu_export_local_gpu);
	if (!export_fn) {
		pr_info("sev_gpu: nvidia GPU-identity export absent; client enumeration disabled\n");
		return;
	}

	memset(&desc, 0, sizeof(desc));
	st = export_fn(&desc, sizeof(desc));
	symbol_put(sev_gpu_export_local_gpu);

	if (st != 0 /* NV_OK */ || !desc.valid) {
		pr_warn("sev_gpu: no local GPU to publish (status=0x%x)\n", st);
		return;
	}

	/* Capture the real GPU's UUID so the first client attach can build the
	 * manager's resident UVM channel manager (sev_gpu_manager_setup_client_channels). */
	memcpy(manager_gpu_uuid, desc.uuid, sizeof(manager_gpu_uuid));
	WRITE_ONCE(manager_gpu_uuid_valid, true);

	memcpy_toio(d->shmem + SEV_GPU_GPUDESC_REGION_OFF, &desc, sizeof(desc));
	wmb();
	pr_info("sev_gpu: published GPU identity gpu_id=0x%08x pci=%04x:%02x:%02x.%x %04x:%04x\n",
		desc.gpu_id, desc.domain, desc.bus, desc.slot, desc.function,
		desc.vendor_id, desc.device_id);
}

/*
 * Client: read the published descriptor and register a synthetic /dev/nvidiaN.
 *
 * Called both at probe and (as a retry) from the RM-forward path, because the
 * manager may publish its GPU identity after this client bound. It is therefore
 * idempotent and race-safe: @client_gpu_registered gives a cheap steady-state
 * early-out, and @attach_busy serializes the one-time register so concurrent
 * CUDA threads can't double-register (the singleton synthetic device).
 */
void sev_gpu_client_attach_gpu(struct sev_gpu_dev *d)
{
	static atomic_t attach_busy = ATOMIC_INIT(0);
	sev_gpu_client_register_gpu_t reg_fn;
	sev_gpu_card_desc_t desc;
	u32 st;

	if (READ_ONCE(client_gpu_registered))
		return;
	if (!d || !d->shmem)
		return;
	if (SEV_GPU_GPUDESC_REGION_OFF + sizeof(desc) > d->shmem_size)
		return;
	if (atomic_cmpxchg(&attach_busy, 0, 1) != 0)
		return;
	if (READ_ONCE(client_gpu_registered))
		goto out_unbusy;

	memcpy_fromio(&desc, d->shmem + SEV_GPU_GPUDESC_REGION_OFF, sizeof(desc));
	rmb();

	if (desc.version != SEV_GPU_CARD_DESC_VERSION || !desc.valid) {
		pr_info_once("sev_gpu: no GPU identity published yet; /dev/nvidia0 deferred until manager publishes\n");
		goto out_unbusy;
	}

	reg_fn = symbol_get(sev_gpu_client_register_gpu);
	if (!reg_fn) {
		pr_info_once("sev_gpu: nvidia client-register absent; /dev/nvidia0 not created\n");
		goto out_unbusy;
	}
	st = reg_fn(&desc, sizeof(desc));
	symbol_put(sev_gpu_client_register_gpu);

	if (st != 0 /* NV_OK */) {
		pr_warn("sev_gpu: client GPU register failed status=0x%x\n", st);
		goto out_unbusy;
	}
	WRITE_ONCE(client_gpu_registered, true);
	pr_info("sev_gpu: registered synthetic GPU gpu_id=0x%08x for client enumeration\n",
		desc.gpu_id);
out_unbusy:
	atomic_set(&attach_busy, 0);
}

/* Client: tear down the synthetic device at module unbind. */
static void sev_gpu_client_detach_gpu(void)
{
	sev_gpu_client_unregister_gpu_t unreg_fn;

	if (!client_gpu_registered)
		return;

	unreg_fn = symbol_get(sev_gpu_client_unregister_gpu);
	if (unreg_fn) {
		unreg_fn();
		symbol_put(sev_gpu_client_unregister_gpu);
	}
	client_gpu_registered = false;
}

/* ------------------------------------------------------------------ */
/* PCI driver                                                          */
/* ------------------------------------------------------------------ */

static void sev_gpu_free_irqs(struct sev_gpu_dev *d)
{
	int i;

	for (i = 0; i < d->nvectors; i++)
		free_irq(pci_irq_vector(d->pdev, i), &d->irqctx[i]);
	if (d->nvectors)
		pci_free_irq_vectors(d->pdev);
	d->nvectors = 0;
}

static int sev_gpu_setup_irqs(struct sev_gpu_dev *d)
{
	int ret, i, j;

	ret = pci_alloc_irq_vectors(d->pdev, IVSHMEM_NUM_VECTORS,
				    IVSHMEM_NUM_VECTORS, PCI_IRQ_MSIX);
	if (ret < 0) {
		pr_warn("sev_gpu: MSI-X unavailable (%d); notifications via local/poll only\n",
			ret);
		d->nvectors = 0;
		return 0;	/* not fatal: fall back to local wakeups */
	}

	for (i = 0; i < IVSHMEM_NUM_VECTORS; i++) {
		d->irqctx[i].dev = d;
		d->irqctx[i].vector = i;
		ret = request_irq(pci_irq_vector(d->pdev, i),
				  sev_gpu_irq_handler, 0, DRV_NAME,
				  &d->irqctx[i]);
		if (ret) {
			pr_err("sev_gpu: request_irq vector %d failed (%d)\n",
			       i, ret);
			for (j = 0; j < i; j++)
				free_irq(pci_irq_vector(d->pdev, j),
					 &d->irqctx[j]);
			pci_free_irq_vectors(d->pdev);
			return ret;
		}
	}
	d->nvectors = IVSHMEM_NUM_VECTORS;
	return 0;
}

/* ivshmem-doorbell (control) exposes MSI-X; ivshmem-plain (data) does not. */
static bool pdev_is_control(struct pci_dev *pdev)
{
	return pci_find_capability(pdev, PCI_CAP_ID_MSIX) != 0;
}

/* ---- per-VM private data device probe (ivshmem-plain, BAR2 only) ---- */
static int sev_gpu_probe_data(struct pci_dev *pdev)
{
	struct sev_gpu_data_dev *dd;
	int idx, ret;

	mutex_lock(&reg_lock);
	idx = num_data_devs;
	if (idx >= SEV_GPU_MAX_VMS) {
		mutex_unlock(&reg_lock);
		pr_warn("sev_gpu: too many data devices (max %d)\n",
			SEV_GPU_MAX_VMS);
		return -ENOSPC;
	}

	dd = kzalloc(sizeof(*dd), GFP_KERNEL);
	if (!dd) {
		mutex_unlock(&reg_lock);
		return -ENOMEM;
	}

	dd->pdev = pdev;
	dd->is_manager = manager;
	dd->pool_index = idx;
	pci_set_drvdata(pdev, dd);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret)
		goto err_disable;

	dd->mem_phys = pci_resource_start(pdev, 2);
	dd->mem_size = pci_resource_len(pdev, 2);
	if (!dd->mem_phys || !dd->mem_size) {
		pr_err("sev_gpu: data dev BAR2 not present\n");
		ret = -ENODEV;
		goto err_regions;
	}

	dd->mem = pci_iomap(pdev, 2, dd->mem_size);
	if (!dd->mem) {
		ret = -ENOMEM;
		goto err_regions;
	}

	if (dd->is_manager)
		data_init_header(dd);

	ret = sev_gpu_data_setup_chardev(dd);
	if (ret)
		goto err_unmap;

	data_devs[idx] = dd;
	num_data_devs = idx + 1;
	mutex_unlock(&reg_lock);

	pr_info("sev_gpu: data[%u] bound phys=0x%llx size=%zu manager=%d -> /dev/sev_gpu_data%u\n",
		dd->pool_index, (unsigned long long)dd->mem_phys,
		dd->mem_size, dd->is_manager, dd->pool_index);
	return 0;

err_unmap:
	pci_iounmap(pdev, dd->mem);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	pci_set_drvdata(pdev, NULL);
	kfree(dd);
	mutex_unlock(&reg_lock);
	return ret;
}

/* ---- control device probe (ivshmem-doorbell: BAR0 regs + MSI-X + BAR2) ---- */
static int sev_gpu_probe_control(struct pci_dev *pdev)
{
	struct sev_gpu_dev *d;
	int ret;

	if (ctrl_dev) {
		pr_warn("sev_gpu: a control device is already bound\n");
		return -EBUSY;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->pdev = pdev;
	d->is_manager = manager;
	d->client_vm_id = 0;
	d->comm_vm_id = 0;
	init_waitqueue_head(&d->grant_wq);
	mutex_init(&d->rpc_lock);
	mutex_init(&d->copy_lock);
	atomic_set(&d->mgr_polling, 0);
	atomic_set(&d->cli_polling, 0);
	atomic_set(&d->rpc_seq, 0);
	pci_set_drvdata(pdev, d);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret)
		goto err_disable;

	d->regs = pci_iomap(pdev, 0, 0x100);
	if (!d->regs) {
		ret = -ENOMEM;
		goto err_regions;
	}

	d->shmem_phys = pci_resource_start(pdev, 2);
	d->shmem_size = pci_resource_len(pdev, 2);
	if (!d->shmem_phys || !d->shmem_size) {
		pr_err("sev_gpu: BAR2 (shared memory) not present\n");
		ret = -ENODEV;
		goto err_unmap_regs;
	}

	d->shmem = pci_iomap(pdev, 2, d->shmem_size);
	if (!d->shmem) {
		ret = -ENOMEM;
		goto err_unmap_regs;
	}

	d->ivposition = ioread32(d->regs + IVSHMEM_REG_IVPOSITION);

	spin_lock_init(&manager_state.lock);
	manager_state.gpu_owner = 0xFF;
	spin_lock_init(&comm_keystore.lock);
	spin_lock_init(&assign_state.lock);
	spin_lock_init(&cc_pool.lock);
	spin_lock_init(&client_kmb_store.lock);

	/* Automatic-handshake worker (both roles): kicked from SET_COMM_KEY. */
	spin_lock_init(&hs_state.lock);
	hs_state.pending = 0;
	INIT_WORK(&hs_state.work, sev_gpu_hs_work);
	hs_state.wq = alloc_workqueue("sev_gpu_hs", WQ_UNBOUND, 1);
	if (!hs_state.wq) {
		ret = -ENOMEM;
		goto err_unmap_shmem;
	}

	if (d->is_manager) {
		d->sched_wq = alloc_workqueue("sev_gpu_sched", WQ_UNBOUND, 1);
		if (!d->sched_wq) {
			ret = -ENOMEM;
			goto err_wq;
		}
		INIT_WORK(&d->sched_work, sev_gpu_sched_work);

		d->rpc_wq = alloc_workqueue("sev_gpu_rpc", WQ_UNBOUND, 1);
		if (!d->rpc_wq) {
			destroy_workqueue(d->sched_wq);
			d->sched_wq = NULL;
			ret = -ENOMEM;
			goto err_wq;
		}
		INIT_WORK(&d->rpc_work, sev_gpu_rpc_work);

		manager_init_layout(d);
	} else {
		/* Best-effort: manager may not have initialized yet. */
		if (client_read_layout(d) == -EAGAIN)
			pr_info("sev_gpu: manager header not ready; will retry on ioctl\n");
		/* Adopt ivposition as our slot id up front so RM-RPC works even
		 * before a REGISTER_VM ioctl (e.g. the rpc_test self-test). */
		d->client_vm_id = (d->ivposition > 0 &&
				    d->ivposition < SEV_GPU_MAX_VMS) ?
					(u8)d->ivposition : 0;
	}

	ret = sev_gpu_setup_irqs(d);
	if (ret)
		goto err_wq;

	ret = sev_gpu_setup_chardev(d);
	if (ret)
		goto err_irqs;

	ctrl_dev = d;
	pr_info("sev_gpu: bound ivshmem ivpos=%d shmem_phys=0x%llx size=%zu manager=%d vectors=%d\n",
		d->ivposition, (unsigned long long)d->shmem_phys,
		d->shmem_size, d->is_manager, d->nvectors);
	pr_info("sev_gpu: /dev/%s ready\n", DEVICE_NAME);

	/* Bring up the RM-RPC control plane for this role. */
	if (d->is_manager) {
		rpc_manager_start();
		rpc_manager_bind_nvidia();
		kmb_manager_bind_nvidia();
		sev_gpu_compute_selftest();
		sev_gpu_publish_gpu_desc(d);
	} else {
		rpc_client_bind_nvidia();
		sev_gpu_client_attach_gpu(d);
	}
	return 0;

err_irqs:
	sev_gpu_free_irqs(d);
err_wq:
	if (d->rpc_wq)
		destroy_workqueue(d->rpc_wq);
	if (d->sched_wq)
		destroy_workqueue(d->sched_wq);
	if (hs_state.wq) {
		destroy_workqueue(hs_state.wq);
		hs_state.wq = NULL;
	}
err_unmap_shmem:
	pci_iounmap(pdev, d->shmem);
err_unmap_regs:
	pci_iounmap(pdev, d->regs);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	pci_set_drvdata(pdev, NULL);
	kfree(d);
	return ret;
}

static void sev_gpu_remove_control(struct pci_dev *pdev)
{
	struct sev_gpu_dev *d = pci_get_drvdata(pdev);

	if (!d)
		return;

	sev_gpu_teardown_chardev(d);
	/* Tear down the RM-RPC control plane before releasing IRQs/memory. */
	if (d->is_manager) {
		rpc_manager_stop();
		rpc_manager_unbind_nvidia();
		kmb_manager_unbind_nvidia();
	} else {
		sev_gpu_client_detach_gpu();
		rpc_client_unbind_nvidia();
	}
	/* Drain the poller before freeing IRQs: a queued bottom half may still
	 * re-arm (enable_irq) a vector, which must not race with free_irq. */
	if (d->rpc_wq) {
		flush_workqueue(d->rpc_wq);
		destroy_workqueue(d->rpc_wq);
		d->rpc_wq = NULL;
	}
	if (d->sched_wq) {
		flush_workqueue(d->sched_wq);
		destroy_workqueue(d->sched_wq);
		d->sched_wq = NULL;
	}
	if (hs_state.wq) {
		flush_workqueue(hs_state.wq);
		destroy_workqueue(hs_state.wq);
		hs_state.wq = NULL;
	}
	sev_gpu_free_irqs(d);
	pci_iounmap(pdev, d->shmem);
	pci_iounmap(pdev, d->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	ctrl_dev = NULL;
	kfree(d);

	pr_info("sev_gpu: control device removed\n");
}

static void sev_gpu_remove_data(struct pci_dev *pdev)
{
	struct sev_gpu_data_dev *dd = pci_get_drvdata(pdev);
	unsigned int pool_index;

	if (!dd)
		return;

	pool_index = dd->pool_index;

	mutex_lock(&reg_lock);
	if (pool_index < SEV_GPU_MAX_VMS && data_devs[pool_index] == dd)
		data_devs[pool_index] = NULL;
	mutex_unlock(&reg_lock);

	sev_gpu_data_teardown_chardev(dd);
	pci_iounmap(pdev, dd->mem);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(dd);

	pr_info("sev_gpu: data[%u] removed\n", pool_index);
}

static int sev_gpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	if (pdev_is_control(pdev))
		return sev_gpu_probe_control(pdev);
	return sev_gpu_probe_data(pdev);
}

static void sev_gpu_remove(struct pci_dev *pdev)
{
	if (pdev_is_control(pdev))
		sev_gpu_remove_control(pdev);
	else
		sev_gpu_remove_data(pdev);
}

static const struct pci_device_id sev_gpu_ids[] = {
	{ PCI_DEVICE(IVSHMEM_VENDOR_ID, IVSHMEM_DEVICE_ID) },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, sev_gpu_ids);

static struct pci_driver sev_gpu_pci_driver = {
	.name     = DRV_NAME,
	.id_table = sev_gpu_ids,
	.probe    = sev_gpu_probe,
	.remove   = sev_gpu_remove,
};

static void rpc_client_bind_nvidia(void)
{
	void (*reg)(sev_gpu_rm_forwarder_t);

	reg = symbol_get(sev_gpu_register_rm_forwarder);
	if (!reg) {
		pr_info("sev_gpu: nvidia RM hook absent; RM-RPC idle until bound\n");
		return;
	}
	rpc_unregister_forwarder = symbol_get(sev_gpu_unregister_rm_forwarder);
	reg(sev_gpu_rm_forward);
	symbol_put(sev_gpu_register_rm_forwarder);
	pr_info("sev_gpu: registered RM forwarder with nvidia.ko\n");

	{
		void (*reg_inst)(sev_gpu_kmb_install_t) =
			symbol_get(sev_gpu_register_kmb_install);
		if (reg_inst) {
			kmb_install_unregister_fn =
				symbol_get(sev_gpu_unregister_kmb_install);
			reg_inst(sev_gpu_kmb_install_impl);
			symbol_put(sev_gpu_register_kmb_install);
			pr_info("sev_gpu: registered GET_KMB install callback with nvidia.ko\n");
		}
	}

	mmap_client_bind_nvidia();
	sev_gpu_usermode_timer_start();
}

static void rpc_client_unbind_nvidia(void)
{
	sev_gpu_usermode_timer_stop();
	mmap_client_unbind_nvidia();

	if (kmb_install_unregister_fn) {
		kmb_install_unregister_fn();
		symbol_put(sev_gpu_unregister_kmb_install);
		kmb_install_unregister_fn = NULL;
	}

	if (rpc_unregister_forwarder) {
		rpc_unregister_forwarder();
		symbol_put(sev_gpu_unregister_rm_forwarder);
		rpc_unregister_forwarder = NULL;
	}
}

static int __init sev_gpu_init(void)
{
	int ret;

	/* One shared minor region + class for the control device (minor 0)
	 * and up to SEV_GPU_MAX_VMS data devices (minors 1..N). */
	ret = alloc_chrdev_region(&sev_gpu_devt_base, 0, SEV_GPU_MINORS,
				  DEVICE_NAME);
	if (ret)
		return ret;

	sev_gpu_class = class_create(CLASS_NAME);
	if (IS_ERR(sev_gpu_class)) {
		ret = PTR_ERR(sev_gpu_class);
		goto err_region;
	}

	ret = pci_register_driver(&sev_gpu_pci_driver);
	if (ret)
		goto err_class;
	return 0;

err_class:
	class_destroy(sev_gpu_class);
err_region:
	unregister_chrdev_region(sev_gpu_devt_base, SEV_GPU_MINORS);
	return ret;
}

static void __exit sev_gpu_exit(void)
{
	/* Drain any queued per-client setup, then balance the retains it took on the
	 * resident manager channel manager, before the providing module unloads. */
	cancel_work_sync(&client_setup_work);
	sev_gpu_manager_release_all_clients();
	pci_unregister_driver(&sev_gpu_pci_driver);
	class_destroy(sev_gpu_class);
	unregister_chrdev_region(sev_gpu_devt_base, SEV_GPU_MINORS);
}

module_init(sev_gpu_init);
module_exit(sev_gpu_exit);
