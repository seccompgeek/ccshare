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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martin");
MODULE_DESCRIPTION("SEV GPU Manager - ivshmem-doorbell cross-VM GPU scheduling");
MODULE_VERSION("0.2");

#define DRV_NAME    "sev_gpu_manager"
#define DEVICE_NAME "sev_gpu_manager"
#define CLASS_NAME  "sev_gpu"

#define SHMEM_MAGIC 0xDEADBEEFCAFEBABEULL

/* Role: manager (default) initializes shared memory and runs the scheduler. */
static bool manager = true;
module_param(manager, bool, 0444);
MODULE_PARM_DESC(manager, "1 = act as GPU manager (default), 0 = act as client");

/*
 * RM-RPC loopback (manager only, transport test): when set, the manager echoes
 * each forwarded RM escape back to the client without touching a GPU. Lets the
 * forwarding path be exercised on an ordinary VM with no GPU/nvidia.ko.
 */
static bool rpc_loopback;
module_param(rpc_loopback, bool, 0444);
MODULE_PARM_DESC(rpc_loopback, "manager: echo RM-RPC requests without a GPU (transport test)");

/*
 * Manager: complete mediated CE secure-copy requests without a real GPU. The
 * ownership check and offset/alignment framing are still fully enforced; only
 * the GPU Copy Engine submit is skipped (reported as success). Lets the
 * client->manager REQUEST_COPY transport be exercised on a VM with no GPU.
 */
static bool copy_loopback;
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
static bool enforce_channel_ownership = true;
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
static bool bringup_ring = true;
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

static uint hs_keyspace;
module_param(hs_keyspace, uint, 0644);
MODULE_PARM_DESC(hs_keyspace, "manager: LCE keyspace to auto-assign on handshake (default 0)");

static uint hs_timeout_ms = 120000;
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
static bool auto_mtls = true;
module_param(auto_mtls, bool, 0644);
MODULE_PARM_DESC(auto_mtls, "run the in-kernel ECDHE-PSK handshake to establish the comm key on first client<->manager contact (default 1)");

static char *psk_path = "/home/martin/sev-gpu/userspace/certs/psk.bin";
module_param(psk_path, charp, 0644);
MODULE_PARM_DESC(psk_path, "path to the 32-byte shared PSK file anchoring the in-kernel handshake");

static uint auto_mtls_wait_ms = 4000;
module_param(auto_mtls_wait_ms, uint, 0644);
MODULE_PARM_DESC(auto_mtls_wait_ms, "in-kernel handshake per-phase / comm-key wait in ms (default 4000, keep < RPC timeout)");

/* Per-vector interrupt context (passed as request_irq dev_id). */
struct sev_gpu_irq {
	struct sev_gpu_dev *dev;
	int vector;
};

/* The shared CONTROL ivshmem device (ivshmem-doorbell: has MSI-X + BAR0 regs). */
struct sev_gpu_dev {
	struct pci_dev *pdev;

	void __iomem *regs;          /* BAR0: device registers   */
	void __iomem *shmem;         /* BAR2: shared memory (kva) */
	phys_addr_t   shmem_phys;    /* BAR2 physical address     */
	size_t        shmem_size;    /* BAR2 length               */

	int  ivposition;             /* this peer's ivshmem id    */
	bool is_manager;             /* role                      */
	u8   client_vm_id;           /* client: our slot index    */
	u8   comm_vm_id;             /* client: logical id from the keybroker
	                              * (manager<->client comm key + KMB slot) */

	int  nvectors;
	struct sev_gpu_irq irqctx[IVSHMEM_NUM_VECTORS];

	/* Cached shared-memory layout (offsets within BAR2). */
	u64 request_off;
	u64 grant_off;
	u64 data_off;
	u32 per_vm_data;

	/* Control-plane synchronization. */
	wait_queue_head_t grant_wq;  /* client: grant arrived     */
	struct workqueue_struct *sched_wq;
	struct work_struct sched_work;

	/* RM-RPC mailbox transport. */
	struct workqueue_struct *rpc_wq;   /* manager: replay worker          */
	struct work_struct rpc_work;       /* manager: scan client mailboxes   */
	struct mutex rpc_lock;             /* client: serialize one in-flight  */
	atomic_t rpc_seq;                  /* client: request sequence number  */
	struct mutex copy_lock;            /* client: serialize one copy job   */

	/* Interrupt-mitigation (NAPI-style) state. */
	atomic_t mgr_polling;        /* manager: draining queue, IRQs masked */
	atomic_t cli_polling;        /* client: polling grant after first IRQ */

	/* Character device (minor 0). */
	dev_t devt;
	struct cdev cdev;
	struct device *device;
};

/*
 * A PRIVATE per-VM DATA ivshmem device (ivshmem-plain: BAR2 only, no MSI-X).
 * The manager binds one per client; a client binds exactly its own. The
 * region's first page holds a sev_gpu_data_header_t; the payload follows.
 */
struct sev_gpu_data_dev {
	struct pci_dev *pdev;
	void __iomem *mem;           /* BAR2: the private region (kva) */
	phys_addr_t   mem_phys;
	size_t        mem_size;
	bool          is_manager;
	u32           pool_index;    /* probe order on this VM */

	dev_t devt;                  /* minor 1 + pool_index */
	struct cdev cdev;
	struct device *device;
};

/* Manager-side bookkeeping. */
static struct {
	spinlock_t lock;
	u8 num_vms;
	unsigned long registered;    /* bitmap of seen client vm_ids */
	u8 gpu_owner;                /* current grant holder, 0xFF = none */
} manager_state;

/*
 * Manager<->client communication keys, delivered from userspace after the
 * keybroker's mutual-TLS handshake (SEV_GPU_IOC_SET_COMM_KEY). These never
 * leave the kernel: the driver uses them to seal the in-kernel exchange of the
 * GPU channel key material (CC_KMB) between manager and client. Indexed by
 * vm_id (manager keeps one per client; a client keeps only its own slot).
 */
static struct {
	spinlock_t lock;
	u8 key[SEV_GPU_MAX_VMS][SEV_GPU_COMM_KEY_LEN];
	unsigned long valid;         /* bitmap of vm_ids holding a key */
} comm_keystore;

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
#define SEV_GPU_MAX_CHANNELS_PER_VM 32

/* Channel kind recorded per assignment (selects the submission datapath). */
#define SEV_GPU_CHAN_KIND_CE       0   /* Copy-Engine channel (cc_pool)        */
#define SEV_GPU_CHAN_KIND_COMPUTE  1   /* GR compute channel (allocate-on-assign) */

struct sev_gpu_assignment {
	bool   in_use;
	u32    kind;                 /* SEV_GPU_CHAN_KIND_*                  */
	u32    channel_id;
	u32    keyspace;
	u32    h_client;             /* GPU client handle  (filled in D4.2) */
	u32    h_channel;            /* GPU channel handle (filled in D4.2) */
	u32    generation;          /* current KMB epoch (last SEND_KMB seq) */
	u64    userd_off;            /* compute: USERD offset in VM DATA region  */
	u64    gpfifo_off;           /* compute: GPFIFO ring offset in DATA region */
	u64    pushbuf_off;          /* compute: method pushbuffer offset in DATA */
	u64    pushbuf_gpu_va;       /* compute: GPU VA for GP_ENTRY address      */
	u64    enc_off;              /* compute: enc region offset in DATA region    */
	u32    gp_put;               /* compute: next free GPFIFO slot / GP_PUT value */
	struct sev_cc_kmb kmb;       /* placeholder in D4.1, real in D4.2    */
};

static struct {
	spinlock_t lock;
	struct sev_gpu_assignment a[SEV_GPU_MAX_VMS][SEV_GPU_MAX_CHANNELS_PER_VM];
} assign_state;


/*
 * D4.2b provisioner pool: the manager pre-allocates confidential-compute
 * channels on the real GPU (each its own kernel RM client) and hands them out
 * from here on ASSIGN. Clients never allocate channels; the manager is the
 * sole allocator.
 */
#define SEV_GPU_CC_POOL_MAX 32

struct sev_gpu_cc_chan {
	bool provisioned;            /* allocated on the GPU                 */
	bool in_use;                 /* currently assigned to a client       */
	u32  h_client;               /* GPU client handle (one per channel)  */
	u32  h_channel;              /* GPU channel handle                   */
	u32  keyspace;               /* CE/LCE keyspace                      */
	u32  channel_id;            /* id surfaced to the client            */
	u8   owner_vm;               /* assignee when in_use                 */
};

static struct {
	spinlock_t lock;
	struct sev_gpu_cc_chan e[SEV_GPU_CC_POOL_MAX];
} cc_pool;

/*
 * Client-side installed channel KMBs (D4.3b). A client stores the unsealed
 * CC_KMB for each channel the manager hands it, and uses it to AES-256-GCM the
 * data it stages in the shared bounce region. The KMB never leaves the kernel.
 * Per-bundle message counters feed the CC IV scheme (gcm_iv = counter ^ ivMask).
 */
struct sev_client_chan {
	bool valid;
	u32  channel_id;
	u32  keyspace;
	u32  generation;             /* KMB epoch (seq of the install)          */
	struct sev_cc_kmb kmb;
	u64  ctr_h2d;                /* encrypt (host->device) message counter */
	u64  ctr_d2h;                /* encrypt counter for the d2h bundle      */
};

static struct {
	spinlock_t lock;
	struct sev_client_chan c[SEV_GPU_MAX_CHANNELS_PER_VM];
} client_kmb_store;

/* AAD that binds each sealed KMB to its (vm, channel, keyspace, seq). */
struct sev_gpu_kmb_aad {
	__u32 magic;
	__u32 vm_id;
	__u32 channel_id;
	__u32 keyspace;
	__u32 seq;
} __packed;

/* The one control device, and the pool of data devices on this VM. */
static struct sev_gpu_dev *ctrl_dev;
static struct sev_gpu_data_dev *data_devs[SEV_GPU_MAX_VMS];

/*
 * Automatic KMB-handshake worker. Triggered by SEV_GPU_IOC_SET_COMM_KEY once
 * the keybroker has delivered the comm key: the manager assigns a channel +
 * seals its KMB, the client receives + installs it. Blocking waits (up to
 * hs_timeout_ms) run here, off the ioctl path. One control device per VM, so a
 * single global worker suffices; the manager tracks pending clients in a bitmap.
 */
static struct {
	struct workqueue_struct *wq;
	struct work_struct       work;
	spinlock_t               lock;
	unsigned long            pending;   /* manager: vm_ids awaiting handshake */
} hs_state;
static int num_data_devs;
/* Client-mode: shadow doorbell PFN stored on MAP_MEMORY reply, consumed by mmap redirect. */
static unsigned long doorbell_mmap_pfn;
/*
 * Manager-mode: per-client cache of the client's data-region BAR2 GPA,
 * populated from the RPC slot's client_data_phys field on every request.
 * Avoids depending on the client writing to the shared data-device header
 * (which fails if the client has no data device or if SEV-SNP C-bit issues
 * prevent the data-header write from being visible to the manager).
 */
static u64 client_mem_phys_cache[SEV_GPU_MAX_VMS];
/*
 * Manager-mode: per-client bump cursor (region-relative byte offset) for the
 * OS-descriptor reserve. CUDA registers small CPU buffers of its own as GPU
 * memory via esc 0x27 (NV01_MEMORY_SYSTEM_OS_DESCRIPTOR); the manager backs each
 * with a fresh page-aligned slice of that client's ivshmem region carved here.
 * Bump-only (never freed) -- the proof workload registers a handful of buffers.
 */
static u64 osdesc_carve_cursor[SEV_GPU_MAX_VMS];
/*
 * DIAG (read-only): region-relative offset of the most recent >=2 MiB osdesc
 * carve per client -- the "0x3e" compute sysmem pool CUDA faults on. Lets the
 * manager replay path log its OWN copy of the control word at pool+0x3fffc so
 * we can tell whether the real driver ever writes it (coherency vs bring-up).
 */
static u64 osdesc_2m_off[SEV_GPU_MAX_VMS];
/*
 * DIAG (read-only): region-relative offset of the channel USERD/GPFIFO buffer
 * -- the FIRST >=2 MiB osdesc carve of a run, backed shared for the client
 * (Task A). Because the ring/USERD are now CPU-visible to the manager, the
 * replay path can inspect whether CUDA ever writes a GPFIFO entry + GP_PUT into
 * it, i.e. whether it attempts a work submit at all. Latched on first 2 MiB
 * carve, cleared on pool reclaim.
 */
static u64 userd_2m_off[SEV_GPU_MAX_VMS];
static DEFINE_MUTEX(reg_lock);

/* Shared chardev infrastructure: minor 0 = control, minors 1.. = data. */
#define SEV_GPU_MINORS	(1 + SEV_GPU_MAX_VMS)
static struct class *sev_gpu_class;
static dev_t sev_gpu_devt_base;

/* ------------------------------------------------------------------ */
/* RM-RPC (Phase D1): control-plane forwarding of nvidia RM escape ioctls */
/* ------------------------------------------------------------------ */

/* Forwarder installed into nvidia.ko's escape hook (client side). */
typedef u32 (*sev_gpu_rm_forwarder_t)(u32 cmd, void *arg, u32 size);
/* Replay handler exported by nvidia.ko to run an escape on the real GPU. */
typedef u32 (*sev_gpu_rm_replay_t)(u32 client_id, u32 cmd, void *arg, u32 size);
/* CC_KMB fetch exported by nvidia.ko to read a channel's key bundle (D4.2). */
typedef u32 (*sev_gpu_kmb_fetch_t)(u32 h_client, u32 h_channel,
				   void *kmb_out, u32 kmb_size);
/* CC-channel pool allocator/free exported by nvidia.ko (D4.2b provisioner). */
typedef u32 (*sev_gpu_chan_alloc_t)(u32 ce_id, u32 flags,
				    u32 *h_client, u32 *h_channel);
typedef u32 (*sev_gpu_chan_free_t)(u32 h_client);
/* CE secure-copy submit exported by nvidia.ko (D4.3, manager = sole ringer). */
typedef u32 (*sev_gpu_ce_submit_t)(u32 h_client, u32 flags,
				   u64 src, u64 dst, u64 length,
				   u64 auth_tag, u64 iv);
/* Compute doorbell-ring exported by nvidia.ko (todo#7, manager = sole ringer). */
typedef u32 (*sev_gpu_submit_work_t)(u32 h_client, u32 h_channel);
/* Compute work-submit-token query exported by nvidia.ko (Fork B WLC launch). */
typedef u32 (*sev_gpu_get_work_submit_token_t)(u32 h_client, u32 h_channel,
					      u32 *token);
/* Compute (GR) GPFIFO channel pool alloc/free exported by nvidia.ko (Arch B). */
typedef u32 (*sev_gpu_compute_alloc_t)(u32 flags, u64 userd_gpa, u64 gpfifo_gpa,
				       u64 pushbuf_gpa, u32 *h_client,
				       u32 *h_channel, u64 *pushbuf_gpu_va);
typedef u32 (*sev_gpu_compute_free_t)(u32 h_client);
/*
 * Confidential CC_KMB seal (manager) / install (client) callbacks registered
 * INTO nvidia.ko. On GET_KMB the manager fetches the raw KMB from the GPU and
 * calls the seal callback to encrypt it under the per-client comm key before it
 * crosses host-visible ivshmem; the client calls the install callback to unseal
 * it and stash it in the per-channel keystore. Signatures mirror nv.c exactly.
 */
typedef u32 (*sev_gpu_kmb_seal_t)(u32 client_id, u32 channel_id,
				  const void *kmb_plain, u32 kmb_len,
				  void *out_nonce, void *out_tag, void *out_ct,
				  u32 *out_seq, u32 *out_keyspace);
typedef u32 (*sev_gpu_kmb_install_t)(u32 channel_id, u32 seq, u32 keyspace,
				     const void *nonce, const void *tag,
				     const void *ct, u32 ct_len, void *kmb_out);

#define RPC_STATE_OFF	offsetof(sev_gpu_rpc_slot_t, state)
#define RPC_FWD_ERR	0x0000ffffu	/* transport failure (NV_ERR_GENERIC-like) */
#define RPC_TIMEOUT_MS	5000		/* client: max wait for a reply           */
#define RPC_IDLE_POLL_MS	50	/* manager: re-scan cadence when idle      */

/*
 * Base offset (from the start of a per-VM data region) of the zero-copy
 * nested-param staging window -- the last SEV_GPU_RPC_DATA_STAGING_SIZE bytes of
 * the region. Both peers map the same region with the same size, so they agree
 * on the base. Returns 0 if the region is too small to host the window.
 */
static inline u64 rpc_staging_base(size_t mem_size)
{
	return (mem_size > SEV_GPU_RPC_DATA_STAGING_SIZE) ?
		(u64)mem_size - SEV_GPU_RPC_DATA_STAGING_SIZE : 0;
}

/*
 * Per-VM compute-channel reserve (L3.3). Each manager-assigned GR compute
 * channel needs three GPU-DMA-able pages in the assignee's PRIVATE data region:
 * page 0 = USERD (GP_PUT the client publishes), page 1 = the GPFIFO ring, and
 * page 2 = the method pushbuffer that GPFIFO entries point at. Two further pages
 * (page 3 = client-encrypted compute methods = the WLC decrypt source, page 4 =
 * that ciphertext's AES-GCM auth tag) back the Fork B GPU-autonomous launch;
 * they are unused by the plain Option-A (manager-rings-doorbell) datapath. They
 * are backed via OS_PHYS_ADDR in
 * nvidia.ko, so the addresses must be page-aligned and unprotected -- which the
 * ivshmem-plain DATA region (C-bit-clear) is. The reserve sits just below the
 * RPC staging window; like that window it is off-limits to CE bounce traffic.
 */
#define SEV_GPU_COMPUTE_CHAN_STRIDE    (5UL * PAGE_SIZE)   /* USERD + GPFIFO + pushbuf + enc(cipher+tag) */
/*
 * The client maps the WHOLE *_USERMODE_A object, which is the
 * NV_VIRTUAL_FUNCTION register window: dev_vm.h defines
 * NV_VIRTUAL_FUNCTION = 0x0003FFFF:0x00030000, i.e. DRF_SIZE == 0x10000 ==
 * 64 KiB (16 pages), NOT a single page. The doorbell the client writes is at
 * window offset 0x90 (BAR0 0x30090 = base 0x30000 + 0x90) and TIME_0/1 at
 * 0x80/0x84 -- all in page 0 -- but its mmap() still spans the full 64 KiB.
 * Back the entire window with dedicated shadow pages so the client's mapping
 * cannot overlap the adjacent RPC staging window.
 */
#define SEV_GPU_COMPUTE_DOORBELL_PAGES 16UL                /* shadow usermode VF window (64 KiB) */
#define SEV_GPU_COMPUTE_RESERVE_SIZE \
	((u64)SEV_GPU_MAX_CHANNELS_PER_VM * SEV_GPU_COMPUTE_CHAN_STRIDE + \
	 (u64)SEV_GPU_COMPUTE_DOORBELL_PAGES * PAGE_SIZE)

/*
 * Region-relative base offset of the compute reserve (page-aligned). Returns 0
 * if the region cannot host both the RPC staging window and the reserve.
 */
static inline u64 compute_reserve_base(size_t mem_size)
{
	u64 staging = rpc_staging_base(mem_size);

	if (staging <= SEV_GPU_COMPUTE_RESERVE_SIZE)
		return 0;
	return ALIGN_DOWN(staging - SEV_GPU_COMPUTE_RESERVE_SIZE, PAGE_SIZE);
}

/*
 * Region-relative offset of the shadow doorbell page.  It sits at the end of
 * the compute reserve, just past all per-channel USERD/GPFIFO/pushbuf slots.
 */
static inline u64 compute_doorbell_off(size_t mem_size)
{
	u64 base = compute_reserve_base(mem_size);

	if (!base)
		return 0;
	return base + (u64)SEV_GPU_MAX_CHANNELS_PER_VM * SEV_GPU_COMPUTE_CHAN_STRIDE;
}

/*
 * Per-VM OS-descriptor reserve. esc 0x27 with hClass
 * NV01_MEMORY_SYSTEM_OS_DESCRIPTOR registers a CUDA-owned CPU buffer as GPU
 * memory; faithful replay is impossible (the user VA lives in the client
 * process, and kernel RM rejects VA descriptors), so the manager backs each
 * such object with a slice of the client's ivshmem region instead. The reserve
 * sits just below the compute reserve and, like it, is off-limits to CE bounce
 * traffic. It is a monotonic bump pool (see sev_gpu_osdesc_carve_impl) rewound
 * only once the client frees its last carve, so it must hold the workload's
 * entire PEAK live working set within a single run. A full CUDA context bring-up
 * on Blackwell (sm_120) issues ~29 CC-secure channels plus several 2 MiB
 * shared-sysmem (esc-0x3e) buffers and a trailing ~8 MiB one, which peaks just
 * over 16 MiB and exhausted the old 16 MiB reserve (-ENOSPC -> NV_ERR_INSUFFICIENT
 * _RESOURCES on cudaMalloc). Size it at 32 MiB: ~2x the observed peak, while
 * still leaving the region's low ~29 MiB free for client-chosen CE bounce
 * offsets. NB: the client derives the same reserve geometry from this constant
 * (to keep bounce buffers clear of the pool), so both peers MUST build the same
 * value -- rebuild sev_gpu_manager.ko on the client and the manager together.
 */
#define SEV_GPU_OSDESC_RESERVE_SIZE	(32UL * 1024 * 1024)	/* 32 MiB */

/*
 * Region-relative base offset of the OS-descriptor reserve (page-aligned).
 * Returns 0 if the region cannot host it below the compute reserve.
 */
static inline u64 osdesc_reserve_base(size_t mem_size)
{
	u64 cbase = compute_reserve_base(mem_size);

	if (!cbase || cbase <= SEV_GPU_OSDESC_RESERVE_SIZE)
		return 0;
	return ALIGN_DOWN(cbase - SEV_GPU_OSDESC_RESERVE_SIZE, PAGE_SIZE);
}

/*
 * Per-VM WLC/LCIC reserve band (Fork B, Increment 3b step 2). The per-client
 * Work-Launch Channel pool's unprotected pool_sysmem (the encrypted run_push +
 * auth tags the GPU-less client writes) and the paired LCIC pool's pool_sysmem
 * (entry/exit tracking notifiers, also client-written) are backed zero-copy by
 * this client's ivshmem region so the client can write them directly and the
 * GPU DMAs them out of shared RAM. nvidia-uvm.ko imports each over its
 * manager-view GPA (mem_phys + offset) as an OS_PHYS_ADDR descriptor.
 *
 * The bands are TINY: nvidia-uvm sizes WLC pool_sysmem at WLC_SYSMEM_TOTAL_SIZE
 * (416 B) * num_channels and LCIC at sizeof(uvm_gpu_semaphore_notifier_t)
 * (4 B) * 2 * num_channels, with num_channels hard-capped at
 * UVM_CHANNEL_MAX_NUM_CHANNELS_PER_POOL (== UVM_PUSH_MAX_CONCURRENT_PUSHES ==
 * 16). So WLC needs <= 416*16 = 6656 B and LCIC <= 4*2*16 = 128 B. One page for
 * WLC comfortably holds the worst case; use four for headroom, one for LCIC.
 * nvidia-uvm re-validates size >= its exact requirement, so over-reserving here
 * is harmless. This band sits just BELOW the OS-descriptor reserve; like the
 * reserves above it, it is off-limits to CE bounce traffic. Because the unified
 * module derives this geometry identically on both peers, the client and
 * manager sev_gpu_manager.ko MUST be built from the same source (they always
 * are). Existing reserve offsets are UNCHANGED, so the osdesc bump pool and the
 * compute reserve keep their addresses.
 */
#define SEV_GPU_WLC_SYSMEM_SIZE		(4UL * PAGE_SIZE)	/* >= 416 * 16 ch */
#define SEV_GPU_LCIC_SYSMEM_SIZE	(1UL * PAGE_SIZE)	/* >= 4 * 2 * 16 ch */
#define SEV_GPU_WLC_LCIC_RESERVE_SIZE \
	(SEV_GPU_WLC_SYSMEM_SIZE + SEV_GPU_LCIC_SYSMEM_SIZE)

/*
 * Region-relative base offset of the per-VM WLC/LCIC reserve band
 * (page-aligned). The WLC pool_sysmem occupies the low SEV_GPU_WLC_SYSMEM_SIZE
 * bytes and the LCIC pool_sysmem the next SEV_GPU_LCIC_SYSMEM_SIZE bytes.
 * Returns 0 if the region cannot host the band below the OS-descriptor reserve.
 */
static inline u64 wlc_lcic_reserve_base(size_t mem_size)
{
	u64 obase = osdesc_reserve_base(mem_size);

	if (!obase || obase <= SEV_GPU_WLC_LCIC_RESERVE_SIZE)
		return 0;
	return ALIGN_DOWN(obase - SEV_GPU_WLC_LCIC_RESERVE_SIZE, PAGE_SIZE);
}

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
static int sev_gpu_compute_carve(struct sev_gpu_data_dev *dd, u32 idx,
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
#define RPC_NV_ESC_RM_CONTROL	0x2Au
#define RPC_NV_ESC_RM_ALLOC	0x2Bu

#define RPC_NVOS54_PARAMS_OFF		16u	/* NVOS54_PARAMETERS.params      */
#define RPC_NVOS54_PARAMSSIZE_OFF	24u	/* NVOS54_PARAMETERS.paramsSize  */
#define RPC_NVOS54_SIZE			32u
#define RPC_NVOS54_FLAGS_OFF		12u	/* NVOS54_PARAMETERS.flags       */
#define RPC_NVOS54_CMD_OFF		 8u	/* NVOS54_PARAMETERS.cmd         */
#define RPC_NVOS54_FLAGS_FINN_SERIALIZED 0x4u	/* params is a FINN blob         */

/*
 * NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION (0x101): the pParams struct embeds
 * three output string pointers. Offsets within the params struct (LP64, 8-byte
 * aligned NvP64 fields):
 *   [0]  sizeOfStrings          NvU32   length of each string buffer
 *   [8]  pDriverVersionBuffer   NvP64   output: driver version string
 *   [16] pVersionBuffer         NvP64   output: version string
 *   [24] pTitleBuffer           NvP64   output: title string
 */
#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION 0x101u
#define BVPAR_SOS_OFF    0u   /* sizeOfStrings field */
#define BVPAR_PDRVVER_OFF 8u  /* pDriverVersionBuffer field */
#define BVPAR_PVER_OFF   16u  /* pVersionBuffer field */
#define BVPAR_PTITLE_OFF 24u  /* pTitleBuffer field */

#define RPC_NVOS21_PALLOC_OFF		16u	/* NVOS21/64.pAllocParms (same)  */
#define RPC_NVOS21_PARAMSSIZE_OFF	24u	/* NVOS21_PARAMETERS.paramsSize  */
#define RPC_NVOS21_SIZE			32u

#define RPC_NVOS64_PRIGHTS_OFF		24u	/* NVOS64_PARAMETERS.pRightsRequested */
#define RPC_NVOS64_PARAMSSIZE_OFF	32u	/* NVOS64_PARAMETERS.paramsSize  */
#define RPC_NVOS64_SIZE			48u
#define RPC_RS_ACCESS_MASK_SIZE		4u	/* sizeof(RS_ACCESS_MASK)        */

/*
 * One nested pointer found inside a top-level escape struct: the 8-byte NvP64
 * field at @ptr_off references @size bytes that must be deep-copied in @dir.
 * Only the CLIENT needs this per-cmd knowledge -- it builds the buffers[] table
 * the (cmd-agnostic) manager then replays.
 */
struct rpc_nested {
	u32 ptr_off;
	u32 size;
	u32 dir;
};

/*
 * Per-RM-control forwarding policy (client side). Most controls are FLAT (no
 * embedded pointers) and ride the flat/FINN path. A control whose pParams embeds
 * further pointers needs explicit level-2 staging (the manager re-points each
 * embedded pointer at the shared staging region before replay). A few controls
 * could instead be answered locally by the client module from a value the
 * manager publishes into shared memory -- RPC_CTRL_LOCAL is the (currently
 * unused) hook for that, so the client does not blindly forward everything.
 */
enum rpc_ctrl_disp {
	RPC_CTRL_FLAT = 0,	/* no embedded pointers: flat/FINN path        */
	RPC_CTRL_LEVEL2,	/* pParams embeds pointers: explicit level-2   */
	RPC_CTRL_LOCAL,		/* answered locally by the client module (hook)*/
};

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
#define RPC_SIZE_FIXED 0xffffffffu
struct rpc_embedded_field {
	u32 pptr_off;
	u32 size_off;
	u32 fixed_size;
	u32 dir;
	u32 elem_size;
};

struct rpc_ctrl_policy {
	u32 ctrl_cmd;
	u8  disp;
	u8  n_fields;
	const struct rpc_embedded_field *fields;
};

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

static const struct rpc_ctrl_policy *rpc_ctrl_policy(u32 ctrl_cmd)
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
static int rpc_nested_layout(u32 cmd, const void *arg, u32 arg_size,
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

static DEFINE_MUTEX(rpc_client_lock);	/* client: one in-flight RM call at a time */
static u32 rpc_client_seq;
static struct task_struct *rpc_kthread;	/* manager: mailbox replay poller          */
static DECLARE_WAIT_QUEUE_HEAD(rpc_wq);
static atomic_t rpc_kick = ATOMIC_INIT(0);
static sev_gpu_rm_replay_t rpc_replay_fn;	/* manager: bound from nvidia.ko    */
static sev_gpu_kmb_fetch_t kmb_fetch_fn;	/* manager: bound from nvidia.ko    */
static sev_gpu_chan_alloc_t chan_alloc_fn;	/* manager: bound from nvidia.ko    */
static sev_gpu_chan_free_t chan_free_fn;	/* manager: bound from nvidia.ko    */
static sev_gpu_ce_submit_t ce_submit_fn;	/* manager: bound from nvidia.ko    */
static sev_gpu_submit_work_t submit_work_fn;	/* manager: bound from nvidia.ko    */
static sev_gpu_get_work_submit_token_t get_work_submit_token_fn; /* bound from nvidia.ko */
static sev_gpu_compute_alloc_t compute_alloc_fn;	/* manager: bound from nvidia.ko */
static sev_gpu_compute_free_t compute_free_fn;	/* manager: bound from nvidia.ko    */
static void (*rpc_replay_teardown)(void);	/* manager: nvidia.ko replay teardown */
static void (*rpc_unregister_forwarder)(void);	/* client: nvidia.ko unbind        */

static u32 sev_gpu_rm_forward(u32 cmd, void *arg, u32 size);

/*
 * The RM-RPC mailbox lives in the SHARED CONTROL BAR, one fixed-stride slot per
 * VM (indexed like req_slot()/grant_slot()). Returns NULL if the control device
 * isn't bound or its BAR is too small to hold this VM's slot.
 */
static inline void __iomem *rpc_ctrl_mailbox(u8 vm)
{
	size_t off;

	if (!ctrl_dev || !ctrl_dev->shmem || vm >= SEV_GPU_MAX_VMS)
		return NULL;
	off = SEV_GPU_RPC_CTRL_REGION_OFF + (size_t)vm * SEV_GPU_RPC_SLOT_STRIDE;
	if (off + SEV_GPU_RPC_SLOT_STRIDE > ctrl_dev->shmem_size)
		return NULL;
	return ctrl_dev->shmem + off;
}

/* Manager: wake the RM-RPC replay thread (called from the RPC doorbell IRQ). */
static inline void rpc_wake_manager(void)
{
	atomic_set(&rpc_kick, 1);
	wake_up_interruptible(&rpc_wq);
}


/* ------------------------------------------------------------------ */
/* Shared-memory helpers                                               */
/* ------------------------------------------------------------------ */

static inline void __iomem *req_slot(struct sev_gpu_dev *d, u8 vm)
{
	return d->shmem + d->request_off + (size_t)vm * sizeof(gpu_request_t);
}

static inline void __iomem *grant_slot(struct sev_gpu_dev *d, u8 vm)
{
	return d->shmem + d->grant_off + (size_t)vm * sizeof(gpu_grant_t);
}

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

/* Ring a peer's doorbell vector (no-op if the device has no registers). */
static inline void ivshmem_ring(struct sev_gpu_dev *d, u16 peer, u16 vector)
{
	if (d->regs)
		writel(IVSHMEM_DOORBELL_VALUE(peer, vector),
		       d->regs + IVSHMEM_REG_DOORBELL);
}

/*
 * Resolve the manager's ivshmem peer id. The manager publishes its own
 * IVPosition in the control header (manager_peer_id), so clients ring the
 * correct doorbell regardless of VM launch order. Falls back to the legacy
 * assumption (peer 0) if the header is not yet populated or is out of range.
 */
static u16 sev_gpu_manager_peer(struct sev_gpu_dev *d)
{
	u32 peer = SEV_GPU_MANAGER_PEER_ID;

	if (d && d->shmem) {
		u64 magic = 0;

		memcpy_fromio(&magic, d->shmem, sizeof(magic));
		if (magic == SHMEM_MAGIC)
			memcpy_fromio(&peer,
				      d->shmem +
				      offsetof(sev_gpu_shmem_header_t,
					       manager_peer_id),
				      sizeof(peer));
	}
	if (peer >= SEV_GPU_MAX_VMS)
		peer = SEV_GPU_MANAGER_PEER_ID;
	return (u16)peer;
}

/*
 * Interrupt-mitigation helpers (NAPI-style). MSI-X vectors are masked at the
 * IRQ layer with disable_irq/enable_irq; the ivshmem IntrMask register only
 * gates legacy INTx, so it is not used here.
 */

/* Manager: mask/unmask the request + release doorbell vectors. */
static void mgr_irq_mask(struct sev_gpu_dev *d)
{
	if (!d->nvectors)
		return;
	disable_irq_nosync(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_NEW_REQUEST));
	disable_irq_nosync(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_RELEASE));
}

static void mgr_irq_unmask(struct sev_gpu_dev *d)
{
	if (!d->nvectors)
		return;
	enable_irq(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_NEW_REQUEST));
	enable_irq(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_RELEASE));
}

/* Client: re-enable the grant doorbell vector after a poll cycle. */
static void cli_irq_rearm(struct sev_gpu_dev *d)
{
	if (d->nvectors && atomic_cmpxchg(&d->cli_polling, 1, 0) == 1)
		enable_irq(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_GRANT_READY));
}

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
 * Greedy grant: scan request slots, grant any pending request, clear it, and
 * notify the requesting client. Round-robin/time-slicing can layer on top of
 * gpu_owner later.
 */
static int sev_gpu_scan_and_grant(struct sev_gpu_dev *d)
{
	gpu_request_t r;
	gpu_grant_t g;
	u64 now;
	int vm, granted = 0;

	for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
		memcpy_fromio(&r, req_slot(d, vm), sizeof(r));
		if (r.msg_type != GPU_REQ_TIME)
			continue;	/* no pending request in this slot */

		now = ktime_get_real_ns();
		memset(&g, 0, sizeof(g));
		g.vm_id          = (u8)vm;
		g.status         = GPU_STATUS_GRANTED;
		g.allocated_us   = r.duration_us;
		g.grant_start_ns = now;
		g.grant_end_ns   = now + (u64)r.duration_us * 1000ULL;
		memcpy_toio(grant_slot(d, vm), &g, sizeof(g));

		/* Consume the request. */
		r.msg_type = 0;
		memcpy_toio(req_slot(d, vm), &r, sizeof(r));

		spin_lock(&manager_state.lock);
		manager_state.gpu_owner = (u8)vm;
		spin_unlock(&manager_state.lock);

		/* Notify the client (interrupt) and any local waiter. */
		if (vm != d->ivposition)
			ivshmem_ring(d, (u16)vm, IVSHMEM_VECTOR_GRANT_READY);
		wake_up_interruptible(&d->grant_wq);

		pr_info("sev_gpu: granted GPU to VM%d for %u us\n",
			vm, r.duration_us);
		granted++;
	}
	return granted;
}

/*
 * Manager bottom half, run as a NAPI-style poller. The IRQ handler masks the
 * request/release vectors and sets mgr_polling before kicking us, so a storm
 * of client doorbells collapses into one polling pass instead of one interrupt
 * each. We keep draining while there is work; once a full pass is empty we
 * re-arm interrupts and do a final scan to close the request-after-last-scan
 * race (re-masking if something raced in).
 */
static void sev_gpu_sched_work(struct work_struct *w)
{
	struct sev_gpu_dev *d = container_of(w, struct sev_gpu_dev, sched_work);

	if (!d->shmem)
		return;

	for (;;) {
		if (sev_gpu_scan_and_grant(d)) {
			cond_resched();
			continue;	/* drained something; look again */
		}

		/* Queue empty. If we masked IRQs to poll, re-arm them now. */
		if (atomic_read(&d->mgr_polling)) {
			mgr_irq_unmask(d);
			atomic_set(&d->mgr_polling, 0);

			/* Close the race: a request may have landed between the
			 * empty scan above and the unmask. If so, take poll mode
			 * again (unless an interrupt already did) and continue. */
			if (sev_gpu_scan_and_grant(d)) {
				if (atomic_cmpxchg(&d->mgr_polling, 0, 1) == 0)
					mgr_irq_mask(d);
				cond_resched();
				continue;
			}
		}
		break;
	}
}

/*
 * Manager per-client UVM channel pools -- provided by nvidia-uvm.ko, bound via
 * symbol_get (no compile-time dependency, mirroring how this module binds
 * nvidia.ko's sev_gpu_* hooks). uvm_sev_manager_create_client_pool() builds the
 * manager's resident UVM channel manager on the first client (the manager runs
 * no CUDA of its own) and adds one per-client CE pool on an idle Copy Engine;
 * uvm_sev_manager_release_gpu() drops one client's hold. The GPU UUID is passed
 * as a raw 16-byte pointer, ABI-compatible with the callee's NvProcessorUuid*.
 */
typedef u32 (*uvm_sev_manager_create_client_pool_t)(const void *gpu_uuid, u32 client_id,
						    u64 wlc_gpa, u64 wlc_size,
						    u64 lcic_gpa, u64 lcic_size);
typedef void (*uvm_sev_manager_release_gpu_t)(void);
extern u32 uvm_sev_manager_create_client_pool(const void *gpu_uuid, u32 client_id,
					      u64 wlc_gpa, u64 wlc_size,
					      u64 lcic_gpa, u64 lcic_size);
extern void uvm_sev_manager_release_gpu(void);

/* The real GPU's UUID, captured when the identity is published (see
 * sev_gpu_publish_gpu_desc), and a bitmap of clients whose per-client pool the
 * manager has already created. */
static u8 manager_gpu_uuid[16];
static bool manager_gpu_uuid_valid;
static unsigned long client_channels_setup;
static unsigned long client_channels_pending;

/*
 * Manager: build this client's per-client UVM channel pool -- and, on the first
 * client, the manager's resident channel manager -- on the real GPU. This is the
 * kernel-triggered, first-client-driven path: the manager owns no workload, so a
 * client attaching is what conjures the channel manager.
 *
 * Triggered once per client from sev_gpu_commit_comm_key() -- i.e. only after the
 * mTLS comm channel is established and the comm KMB is shared -- and dispatched to
 * the setup workqueue (never inline) because uvm_sev_manager_create_client_pool()
 * drives a heavy uvm_gpu_retain_by_uuid (RM device / PMM / channel-manager create)
 * that must not stall the manager poller kthread.
 *
 * The per-client WLC/LCIC pools' unprotected pool_sysmem is backed zero-copy by
 * this client's ivshmem DATA region at the WLC/LCIC reserve band
 * (wlc_lcic_reserve_base): WLC at [base, +SEV_GPU_WLC_SYSMEM_SIZE), LCIC at the
 * next SEV_GPU_LCIC_SYSMEM_SIZE bytes. We pass the manager-view GPAs (mem_phys +
 * offset) that nvidia-uvm imports as OS_PHYS_ADDR descriptors; the client derives
 * the same offsets from the shared reserve geometry. The CE pool needs no GPA.
 */
static void sev_gpu_manager_setup_client_channels(u32 vm_id)
{
	uvm_sev_manager_create_client_pool_t create_fn;
	struct sev_gpu_data_dev *dd;
	u64 band, wlc_gpa, wlc_size, lcic_gpa, lcic_size;
	u32 st;

	if (!manager || vm_id >= SEV_GPU_MAX_VMS)
		return;
	if (!READ_ONCE(manager_gpu_uuid_valid))
		return;

	/* Resolve this client's private DATA region and its WLC/LCIC reserve band. */
	if (vm_id >= (u32)num_data_devs)
		return;
	dd = data_devs[vm_id];
	if (!dd || !dd->mem_phys)
		return;
	band = wlc_lcic_reserve_base(dd->mem_size);
	if (!band) {
		pr_warn("sev_gpu: VM %u data region too small for WLC/LCIC reserve; per-client pools disabled\n",
			vm_id);
		return;
	}
	wlc_gpa   = (u64)dd->mem_phys + band;
	wlc_size  = SEV_GPU_WLC_SYSMEM_SIZE;
	lcic_gpa  = wlc_gpa + SEV_GPU_WLC_SYSMEM_SIZE;
	lcic_size = SEV_GPU_LCIC_SYSMEM_SIZE;

	if (test_and_set_bit(vm_id, &client_channels_setup))
		return;		/* already set up for this client */

	create_fn = symbol_get(uvm_sev_manager_create_client_pool);
	if (!create_fn) {
		pr_info_once("sev_gpu: nvidia-uvm manager channel op absent; per-client pools disabled\n");
		clear_bit(vm_id, &client_channels_setup);
		return;
	}
	st = create_fn(manager_gpu_uuid, vm_id, wlc_gpa, wlc_size, lcic_gpa, lcic_size);
	symbol_put(uvm_sev_manager_create_client_pool);

	if (st != 0 /* NV_OK */) {
		pr_warn("sev_gpu: per-client channel pool create failed for VM %u (status=0x%x)\n",
			vm_id, st);
		clear_bit(vm_id, &client_channels_setup);
		return;
	}
	pr_info("sev_gpu: created per-client UVM channel pool for VM %u\n", vm_id);
}

/* Setup workqueue: build channels for every client queued by
 * sev_gpu_manager_note_client_active(), off the poller kthread. */
static void sev_gpu_manager_setup_work_fn(struct work_struct *work)
{
	unsigned vm_id;

	for (vm_id = 0; vm_id < SEV_GPU_MAX_VMS; vm_id++) {
		if (test_and_clear_bit(vm_id, &client_channels_pending))
			sev_gpu_manager_setup_client_channels(vm_id);
	}
}
static DECLARE_WORK(client_setup_work, sev_gpu_manager_setup_work_fn);

/*
 * Manager: note that a client's comm channel + KMB are established, and queue its
 * per-client UVM channel setup on the workqueue. Idempotent (a client already set
 * up or already queued is ignored). Called from sev_gpu_commit_comm_key(), so it
 * runs after authentication -- never on unauthenticated input.
 */
static void sev_gpu_manager_note_client_active(u32 vm_id)
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

/* Manager: drop the resident channel manager holds taken for every client whose
 * pool we created (balances the per-client retain). Called at module exit. */
static void sev_gpu_manager_release_all_clients(void)
{
	uvm_sev_manager_release_gpu_t release_fn;
	unsigned vm_id;

	if (!manager || client_channels_setup == 0)
		return;

	release_fn = symbol_get(uvm_sev_manager_release_gpu);
	if (!release_fn)
		return;

	for (vm_id = 0; vm_id < SEV_GPU_MAX_VMS; vm_id++) {
		if (test_and_clear_bit(vm_id, &client_channels_setup))
			release_fn();
	}
	symbol_put(uvm_sev_manager_release_gpu);
}

/* Manager: record a client registration. */
static void register_vm(const sev_gpu_ioctl_register_vm_t *reg)
{
	unsigned long flags;

	if (reg->vm_id >= SEV_GPU_MAX_VMS)
		return;

	spin_lock_irqsave(&manager_state.lock, flags);
	if (!(manager_state.registered & (1UL << reg->vm_id))) {
		manager_state.registered |= (1UL << reg->vm_id);
		manager_state.num_vms++;
	}
	spin_unlock_irqrestore(&manager_state.lock, flags);

	if (ctrl_dev && ctrl_dev->shmem)
		iowrite32(manager_state.num_vms,
			  ctrl_dev->shmem + offsetof(sev_gpu_shmem_header_t, num_vms));

	pr_info("sev_gpu: registered VM %d (%s, pid=%d)\n",
		reg->vm_id, reg->vm_name, reg->vm_pid);
}

/* ------------------------------------------------------------------ */
/* RM-RPC mailbox transport (manager replay side)                      */
/* ------------------------------------------------------------------ */

/* Service one client mailbox found in the SEV_GPU_RPC_REQUEST state. */
static void sev_gpu_rpc_service(struct sev_gpu_data_dev *dd)
{
	void __iomem *mb = dd->mem + SEV_GPU_RPC_MAILBOX_OFF;
	sev_gpu_rm_replay_t replay;
	u32 client, arg_size, cmd, n_buffers;
	s32 rm_status;
	s32 ret = 0;

	/*
	 * Authoritative client id is the manager's region->VM mapping
	 * (data-region pool index), never the value a client wrote into shared
	 * memory. It indexes both the per-client RM replay context and the
	 * reply notification.
	 */
	client    = dd->pool_index;
	cmd       = ioread32(mb + offsetof(sev_gpu_rpc_slot_t, cmd));
	arg_size  = ioread32(mb + offsetof(sev_gpu_rpc_slot_t, arg_size));
	n_buffers = ioread32(mb + offsetof(sev_gpu_rpc_slot_t, n_buffers));

	replay = READ_ONCE(rpc_replay_fn);

	if (rpc_loopback) {
		/* Echo: inline_arg is left untouched, just report success. */
		rm_status = 0;
		pr_info("sev_gpu: rpc loopback echo vm=%u cmd=0x%x size=%u\n",
			client, cmd, arg_size);
	} else if (!replay) {
		/* No GPU replay handler bound (nvidia.ko absent / not manager). */
		rm_status = (s32)RPC_FWD_ERR;
		ret = -ENODEV;
		pr_warn_ratelimited("sev_gpu: rpc replay unbound (load nvidia.ko, or rpc_loopback=1)\n");
	} else if (arg_size > SEV_GPU_RPC_INLINE_MAX) {
		rm_status = (s32)RPC_FWD_ERR;
		ret = -EINVAL;
		pr_warn_ratelimited("sev_gpu: rpc arg_size %u exceeds inline max\n",
				    arg_size);
	} else if (n_buffers != 0) {
		/*
		 * Nested-pointer deep copy is not implemented yet: this first
		 * cut forwards flat escapes only (n_buffers == 0).
		 */
		rm_status = (s32)RPC_FWD_ERR;
		ret = -EOPNOTSUPP;
		pr_warn_ratelimited("sev_gpu: rpc nested buffers (%u) unsupported yet\n",
				    n_buffers);
	} else {
		void *argbuf = kzalloc(SEV_GPU_RPC_INLINE_MAX, GFP_KERNEL);

		if (!argbuf) {
			rm_status = (s32)RPC_FWD_ERR;
			ret = -ENOMEM;
		} else {
			/*
			 * Pull the top-level escape struct into a kernel buffer,
			 * replay it on the real GPU under this client's isolated
			 * RM context, then publish the (in/out) result back.
			 */
			if (arg_size)
				memcpy_fromio(argbuf,
					      mb + offsetof(sev_gpu_rpc_slot_t, inline_arg),
					      arg_size);

			rm_status = (s32)replay(client, cmd, argbuf, arg_size);

			if (arg_size)
				memcpy_toio(mb + offsetof(sev_gpu_rpc_slot_t, inline_arg),
					    argbuf, arg_size);
			kfree(argbuf);

			pr_info_ratelimited("sev_gpu: rpc replay vm=%u cmd=0x%x size=%u status=0x%x\n",
					    client, cmd, arg_size, (u32)rm_status);
		}
	}

	iowrite32(rm_status, mb + offsetof(sev_gpu_rpc_slot_t, rm_status));
	iowrite32(ret,       mb + offsetof(sev_gpu_rpc_slot_t, ret));
	wmb();	/* publish payload + status before flipping state */
	iowrite32(SEV_GPU_RPC_REPLY, mb + offsetof(sev_gpu_rpc_slot_t, state));

	/*
	 * DIAG (read-only): after each replay, sample the manager's own copy of
	 * the "0x3e" compute-pool control word at pool+0x3fffc that CUDA faults
	 * on in the client. Non-zero here (while the client reads 0) => a
	 * link/coherency bug; still 0 after the whole construct => the value is
	 * produced by GPU channel bring-up the manager never triggers. Pure
	 * read; does not alter any state.
	 */
	if (client < SEV_GPU_MAX_VMS) {
		u64 o2 = READ_ONCE(osdesc_2m_off[client]);

		if (o2 && o2 + 0x40000 <= (u64)dd->mem_size) {
			u32 w0  = ioread32((u8 __iomem *)dd->mem + o2 + 0x3fffc);
			u32 wa  = ioread32((u8 __iomem *)dd->mem + o2 + 0x3ffc0);
			u32 wb  = ioread32((u8 __iomem *)dd->mem + o2 + 0x3ffd0);

			pr_debug("sev_gpu: DIAG 0x3e vm=%u off=0x%llx word[0x3fffc]=0x%08x [0x3ffc0]=0x%08x [0x3ffd0]=0x%08x (after cmd=0x%x)\n",
				 client, (unsigned long long)o2, w0, wa, wb, cmd);
		}
	}

	/* NAPI-style: the client polls, but kick it so it wakes promptly. */
	if (ctrl_dev && client < SEV_GPU_MAX_VMS)
		ivshmem_ring(ctrl_dev, (u16)client, IVSHMEM_VECTOR_RPC);
}

/*
 * Core mediated CE secure-copy. Verify @vm_id owns @channel_id, translate the
 * payload-relative offsets in that VM's data region into the physical addresses
 * the GPU Copy Engine DMAs, and drive the engine. Shared by the manager-driven
 * SEV_GPU_IOC_SUBMIT_COPY ioctl and the client-driven REQUEST_COPY path.
 *
 * @vm_id MUST be the manager's authoritative region->VM mapping (data-region
 * pool index), never a value a client wrote into shared memory. Returns 0 on
 * success or a negative errno. With no GPU CE bound it succeeds only when
 * copy_loopback=1 (ownership + framing are still fully enforced).
 */
static int sev_gpu_do_ce_copy(u32 vm_id, u32 channel_id, u32 flags,
			      u32 req_generation, u64 src_offset, u64 dst_offset,
			      u64 length, u64 auth_tag_offset, u64 iv_offset)
{
	struct sev_gpu_assignment *slot = NULL;
	struct sev_gpu_data_dev *dd;
	sev_gpu_ce_submit_t submit;
	u32 h_client = 0, st, generation = 0;
	bool h2d;
	u64 sys_off, vram_off, base, payload_sz;
	u64 sys_phys, tag_phys, iv_phys, src_arg, dst_arg;
	int i;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;
	if (length == 0)
		return -EINVAL;

	/*
	 * The bounce buffer lives in the requesting VM's PRIVATE data region
	 * (per-VM ivshmem-plain), not the shared control BAR. Resolve its data
	 * device; the GPU DMAs out of that region's RAM.
	 */
	if (vm_id >= (u32)num_data_devs)
		return -ENXIO;
	dd = data_devs[vm_id];
	if (!dd || !dd->mem_phys || dd->mem_size <= SEV_GPU_DATA_HEADER_SIZE)
		return -ENXIO;
	payload_sz = (u64)dd->mem_size - SEV_GPU_DATA_HEADER_SIZE;

	/*
	 * Ownership enforcement (Option A): a client may only drive a channel
	 * the manager has assigned to it. Reject any channel this VM does not
	 * own BEFORE touching the GPU.
	 */
	spin_lock(&assign_state.lock);
	for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
		struct sev_gpu_assignment *e = &assign_state.a[vm_id][i];

		if (e->in_use && e->channel_id == channel_id) {
			h_client   = e->h_client;
			generation = e->generation;
			slot = e;
			break;
		}
	}
	spin_unlock(&assign_state.lock);
	if (!slot)
		return -EACCES;	/* channel not assigned to this VM */

	/*
	 * D4.3d key-rotation pinning: refuse a request pinned to a stale KMB
	 * epoch (its IV space no longer matches the GPU's).
	 */
	if (req_generation != 0 && req_generation != generation)
		return -ESTALE;

	/*
	 * Translate payload-relative byte offsets. The SYSMEM operand, GCM auth
	 * tag and IV live in this VM's data-region payload (C-bit-clear, after
	 * the 4 KiB header); the FBMEM operand is a byte offset into the
	 * channel's RM-owned VRAM scratch and passes straight through. H2D:
	 * src = sysmem (encrypted input), dst = VRAM. D2H: src = VRAM, dst = sysmem.
	 */
	h2d      = (flags & SEV_GPU_SUBMIT_F_ENCRYPT) != 0;
	sys_off  = h2d ? src_offset : dst_offset;
	vram_off = h2d ? dst_offset : src_offset;

	/* SYSMEM payload, tag and IV must fit within the region payload. */
	if (sys_off > payload_sz || length > payload_sz - sys_off)
		return -EINVAL;
	if (auth_tag_offset > payload_sz - SEV_GPU_BOUNCE_ALIGN ||
	    iv_offset       > payload_sz - SEV_GPU_BOUNCE_ALIGN)
		return -EINVAL;
	/* The CE requires 16-byte-aligned auth-tag and IV addresses. */
	if ((auth_tag_offset & (SEV_GPU_BOUNCE_ALIGN - 1)) ||
	    (iv_offset       & (SEV_GPU_BOUNCE_ALIGN - 1)))
		return -EINVAL;

	submit = READ_ONCE(ce_submit_fn);
	if (!submit || !h_client) {
		if (copy_loopback) {
			/* Transport test: ownership + framing validated, no GPU. */
			pr_info("sev_gpu: copy loopback VM%u ch%u %s %llu bytes (no CE)\n",
				vm_id, channel_id, h2d ? "h2d" : "d2h",
				(unsigned long long)length);
			return 0;
		}
		return -ENODEV;	/* no GPU CE submit bound (placeholder) */
	}

	base     = (u64)dd->mem_phys + SEV_GPU_DATA_HEADER_SIZE;
	sys_phys = base + sys_off;
	tag_phys = base + auth_tag_offset;
	iv_phys  = base + iv_offset;
	src_arg  = h2d ? sys_phys : vram_off;
	dst_arg  = h2d ? vram_off : sys_phys;

	st = submit(h_client, flags, src_arg, dst_arg, length, tag_phys, iv_phys);
	if (st != 0) {	/* 0 == NV_OK */
		pr_warn("sev_gpu: CE submit failed on ch %u (VM%u) status 0x%x\n",
			channel_id, vm_id, st);
		return -EIO;
	}

	pr_info("sev_gpu: VM%u submitted %s copy on channel %u (%llu bytes)\n",
		vm_id, h2d ? "h2d" : "d2h", channel_id,
		(unsigned long long)length);
	return 0;
}

/*
 * Does this VM own (was it assigned) the channel identified by these GPU
 * handles? The manager is the sole channel allocator, so the assignment
 * registry is the authority: a (hClient, hChannel) pair is owned by @vm_id only
 * if it matches an in-use entry the manager recorded for that VM in
 * sev_gpu_assign_channel(). Used to scope work-submit to a VM's own channels.
 */
static bool sev_gpu_vm_owns_channel(u32 vm_id, u32 h_client, u32 h_channel)
{
	bool owned = false;
	int i;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return false;

	spin_lock(&assign_state.lock);
	for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
		struct sev_gpu_assignment *e = &assign_state.a[vm_id][i];

		if (e->in_use && e->h_client == h_client &&
		    e->h_channel == h_channel) {
			owned = true;
			break;
		}
	}
	spin_unlock(&assign_state.lock);
	return owned;
}

/*
 * Ring the doorbell of a client's GPFIFO compute channel on its behalf (todo#7).
 * The manager is the sole doorbell-ringer: the client has already published
 * GP_PUT into its (shared) USERD, so this only nudges the GPU host to re-read
 * the channel's GPFIFO. Returns 0 on success or a negative errno. With no GPU
 * primitive bound it succeeds only when copy_loopback=1 (transport test).
 *
 * @vm_id is the manager's authoritative region->VM mapping (the trusted pool
 * index), never a client-written value.
 *
 * SECURITY NOTE (bring-up): (@h_client, @h_channel) are resolved in the
 * manager's GLOBAL replay RM namespace (g_resServ). The per-client replay
 * contexts share that namespace, so a malicious VM could in principle present
 * another VM's handles. Hardening to scope handles to @vm_id's replay namespace
 * (a per-VM allocated-handle allow-list exposed by the replay layer) is pending,
 * consistent with the rest of the replay path's current trust posture.
 */
static int sev_gpu_do_submit_work(u32 vm_id, u32 h_client, u32 h_channel,
				  bool trusted)
{
	sev_gpu_submit_work_t submit;
	u32 st;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;
	if (!h_client || !h_channel)
		return -EINVAL;

	/*
	 * Per-VM channel scoping: the manager is the sole channel allocator, so
	 * a VM may only ring a doorbell on a channel the manager assigned to it.
	 * Validate (hClient, hChannel) against this VM's assignment registry
	 * before touching the GPU.
	 *
	 * @trusted skips this gate for manager-originated rings whose handles
	 * were captured from the manager's OWN replay of a client channel alloc
	 * (the bring-up doorbell watcher). Such a channel is legitimately not in
	 * the assignment registry (the client allocated it via replay, the
	 * manager did not hand it out), yet the handles are manager-derived, not
	 * client-supplied, so they are safe to ring.
	 */
	if (!trusted && !sev_gpu_vm_owns_channel(vm_id, h_client, h_channel)) {
		if (enforce_channel_ownership) {
			pr_warn("sev_gpu: VM%u submit refused: channel hClient=0x%x hChannel=0x%x not assigned to it\n",
				vm_id, h_client, h_channel);
			return -EACCES;
		}
		/* Bring-up: replay-allocated channel not yet in the registry. */
		pr_warn("sev_gpu: VM%u submit on UNASSIGNED channel hClient=0x%x hChannel=0x%x (ownership enforcement off)\n",
			vm_id, h_client, h_channel);
	}

	submit = READ_ONCE(submit_work_fn);
	if (!submit) {
		if (copy_loopback) {
			/* Transport test: handshake validated, no GPU doorbell. */
			pr_info("sev_gpu: submit-work loopback VM%u hClient=0x%x hChannel=0x%x (no GPU)\n",
				vm_id, h_client, h_channel);
			return 0;
		}
		return -ENODEV;	/* no GPU doorbell-ring primitive bound */
	}

	st = submit(h_client, h_channel);
	if (st != 0) {	/* 0 == NV_OK */
		pr_warn("sev_gpu: work submit failed VM%u hClient=0x%x hChannel=0x%x status 0x%x\n",
			vm_id, h_client, h_channel, st);
		return -EIO;
	}

	pr_info("sev_gpu: VM%u rang doorbell hClient=0x%x hChannel=0x%x\n",
		vm_id, h_client, h_channel);
	return 0;
}

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
#define SEV_GPU_BRINGUP_WATCH_MS   8000u  /* max window to watch after arming   */
#define SEV_GPU_BRINGUP_POLL_MS       2u  /* idle-loop sample period while armed */
#define SEV_GPU_BRINGUP_MAX_RINGS    64u  /* cap rings so a stuck token can't spin */
#define SEV_GPU_BRINGUP_SCAN_BYTES 0x200000u /* scan the full 0x3e pool (2 MiB) */
#define SEV_GPU_BRINGUP_SCAN_MS     1000u /* throttle the heavy pool scan       */
#define SEV_GPU_BRINGUP_SCAN_LOG       8  /* max non-zero dwords logged per scan */
/*
 * USERD ring-pointer offsets (NV_RAMUSERD_*, dev_ram.h -> dword*4): GP_GET is
 * dword 34 (0x88) and GP_PUT is dword 35 (0x8C). CUDA advances GP_PUT@0x8C when
 * it queues a GPFIFO entry -- this is the ring producer, distinct from the
 * usermode doorbell kick at NVC361_NOTIFY_CHANNEL_PENDING (0x90). Logging both
 * from the USERD region reveals whether CUDA published work (GP_PUT moved) even
 * when the +0x90 usermode doorbell never fires (Hopper+/Blackwell CC channels
 * kick via BAR0 NV_VIRTUAL_FUNCTION_DOORBELL 0x30090 instead).
 */
#define SEV_GPU_USERD_GP_GET_OFF   0x88u
#define SEV_GPU_USERD_GP_PUT_OFF   0x8Cu

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
#define SEV_GPU_BRINGUP_MAX_CH       64u
#define SEV_GPU_BRINGUP_MAX_CAND     32u
#define SEV_GPU_BRINGUP_USERD_IN_BUF 0x2000u /* USERD offset inside a shared buf */

struct sev_gpu_bringup_watch {
	bool          active;
	u32           h_client;
	u32           h_channel;
	u32           last_db;    /* last observed doorbell token (+0x90)      */
	u32           last_db_f;  /* last observed 2nd-aperture token (+0xf090) */
	u32           rings;      /* doorbells rung during this watch          */
	unsigned long deadline;   /* jiffies after which the watch expires     */
	unsigned long next_scan;  /* jiffies: next heavy 0x3e-pool write scan   */
};
static struct sev_gpu_bringup_watch bringup_watch[SEV_GPU_MAX_VMS];

/* Per-VM set of every replay GPFIFO channel armed (dedup), for GP_PUT ringing. */
static u32 bringup_ch[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CH][2]; /* [hClient,hChannel] */
static u32 bringup_nch[SEV_GPU_MAX_VMS];
/* Per-VM set of >=2 MiB 0x3e carve region offsets (shared-USERD candidates). */
static u64 bringup_userd_cand[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
static u32 bringup_cand_lastput[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
static u32 bringup_cand_lastget[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
static u32 bringup_ncand[SEV_GPU_MAX_VMS];

/* Arm (or re-arm) the bring-up doorbell watch for a replay channel. */
static void sev_gpu_bringup_arm(u32 vm, u32 h_client, u32 h_channel)
{
	struct sev_gpu_bringup_watch *w;

	if (vm >= SEV_GPU_MAX_VMS || !h_client || !h_channel)
		return;

	w = &bringup_watch[vm];
	w->h_client  = h_client;
	w->h_channel = h_channel;
	w->last_db   = 0;
	w->last_db_f = 0;
	w->rings     = 0;
	w->deadline  = jiffies + msecs_to_jiffies(SEV_GPU_BRINGUP_WATCH_MS);
	w->next_scan = jiffies;   /* scan on the first poll */
	WRITE_ONCE(w->active, true);

	pr_info("sev_gpu: bring-up watch ARMED VM%u hClient=0x%x hChannel=0x%x\n",
		vm, h_client, h_channel);

	/*
	 * Record the channel in the per-VM ring set (dedup). The shared-USERD
	 * channel that actually advances GP_PUT may have been armed earlier and
	 * overwritten in the single-slot watch above, so keep them all here.
	 */
	{
		u32 i, n = READ_ONCE(bringup_nch[vm]);

		for (i = 0; i < n && i < SEV_GPU_BRINGUP_MAX_CH; i++)
			if (bringup_ch[vm][i][0] == h_client &&
			    bringup_ch[vm][i][1] == h_channel)
				return;
		if (n < SEV_GPU_BRINGUP_MAX_CH) {
			bringup_ch[vm][n][0] = h_client;
			bringup_ch[vm][n][1] = h_channel;
			WRITE_ONCE(bringup_nch[vm], n + 1);
		}
	}
}

/* Disarm the watch (e.g. once the client starts issuing STAGED submissions). */
static void sev_gpu_bringup_disarm(u32 vm)
{
	if (vm >= SEV_GPU_MAX_VMS)
		return;
	if (READ_ONCE(bringup_watch[vm].active)) {
		WRITE_ONCE(bringup_watch[vm].active, false);
		pr_info("sev_gpu: bring-up watch disarmed VM%u\n", vm);
	}
}

/*
 * Poll every armed watch once. Returns true if any watch is still active, so
 * the replay poller can shorten its idle sleep and sample the shadow doorbell
 * finely during the bring-up window (a plain memory write gives no wakeup).
 */
static bool sev_gpu_bringup_poll(void)
{
	bool any = false;
	u32 vm;

	for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
		struct sev_gpu_bringup_watch *w = &bringup_watch[vm];
		struct sev_gpu_data_dev *dd;
		u64 db, o2;
		u32 tok, tok_f = 0, sem = 0;

		if (!READ_ONCE(w->active))
			continue;

		if (time_after(jiffies, w->deadline)) {
			WRITE_ONCE(w->active, false);
			pr_info("sev_gpu: bring-up watch expired VM%u (rings=%u)\n",
				vm, w->rings);
			continue;
		}

		dd = ((u32)vm < (u32)num_data_devs) ? data_devs[vm] : NULL;
		if (!dd || !dd->mem)
			continue;

		db = compute_doorbell_off(dd->mem_size);
		if (!db || db + 0xA0 > (u64)dd->mem_size)
			continue;

		any = true;

		/* Shadow usermode doorbell token the client's CUDA writes. */
		tok = ioread32((u8 __iomem *)dd->mem + db + 0x90);

		/*
		 * Diagnostic test: also sample the doorbell word of a possible
		 * SECOND usermode aperture at window offset 0xf090.  The
		 * +0xf080/+0xf084 fields seen live in the full-window scan look
		 * like a second TIME_0/TIME_1 pair, so if CUDA were ringing that
		 * aperture its work-submit token would land at +0xf090 rather
		 * than +0x90.  Read-only: bounds-guarded, logged on advance only,
		 * and never rung on (+0xf090 is not a real HW doorbell).
		 */
		if (db + 0xf094 <= (u64)dd->mem_size)
			tok_f = ioread32((u8 __iomem *)dd->mem + db + 0xf090);

		/*
		 * GP_PUT-advance doorbell ring (experiment). For each shared-USERD
		 * candidate (a >=2 MiB 0x3e carve), read GP_PUT@USERD+0x8C and
		 * GP_GET@USERD+0x88 (USERD at buf+0x2000). When GP_PUT advances past
		 * GP_GET the client has queued GPFIFO entries the GPU has not yet
		 * consumed -- ring every replay channel's real doorbell so the host
		 * re-reads the ring. Edge-triggered per candidate (lastput) to avoid
		 * repeated rings. Bounded GP_PUT sanity (<1024) rejects buffers that
		 * are pushbuffer/bounce data rather than a real USERD.
		 */
		{
			u32 c, nc = READ_ONCE(bringup_ncand[vm]);

			for (c = 0; c < nc && c < SEV_GPU_BRINGUP_MAX_CAND; c++) {
				u64 ub = bringup_userd_cand[vm][c] +
					 SEV_GPU_BRINGUP_USERD_IN_BUF;
				u32 gp_put, gp_get, k, nch;

				if (!bringup_userd_cand[vm][c] ||
				    ub + SEV_GPU_USERD_GP_PUT_OFF + 4 > (u64)dd->mem_size)
					continue;

				gp_put = ioread32((u8 __iomem *)dd->mem + ub +
						  SEV_GPU_USERD_GP_PUT_OFF);
				gp_get = ioread32((u8 __iomem *)dd->mem + ub +
						  SEV_GPU_USERD_GP_GET_OFF);

				/*
				 * Watch GP_GET independently of GP_PUT. After we
				 * ring the doorbell, GP_GET advancing toward
				 * GP_PUT is the ONLY proof the GPU host actually
				 * fetched and consumed the queued GPFIFO entries.
				 * If GP_GET never moves the ring did not reach the
				 * host (wrong token/channel not runnable); if it
				 * reaches GP_PUT the work ran and cuInit must be
				 * blocked on the completion/interrupt path instead.
				 */
				if (gp_get != bringup_cand_lastget[vm][c]) {
					pr_info("sev_gpu: bring-up GP_GET advance VM%u userd@0x%llx GP_GET %u -> %u (GP_PUT=%u)\n",
						vm, (unsigned long long)ub,
						bringup_cand_lastget[vm][c],
						gp_get, gp_put);
					bringup_cand_lastget[vm][c] = gp_get;
				}

				if (gp_put == 0 || gp_put >= 1024u ||
				    gp_put == gp_get ||
				    gp_put == bringup_cand_lastput[vm][c])
					continue;

				bringup_cand_lastput[vm][c] = gp_put;
				nch = READ_ONCE(bringup_nch[vm]);
				pr_info("sev_gpu: bring-up GP_PUT advance VM%u userd@0x%llx GP_GET=%u GP_PUT=%u (%u channel(s), ring=%d)\n",
					vm, (unsigned long long)ub,
					gp_get, gp_put, nch, bringup_ring);

				if (!bringup_ring) {
					/*
					 * Doorbell propagation disabled by module param
					 * (default is ON). The GP_PUT advance is still
					 * logged above for diagnostics.
					 */
					continue;
				}

				for (k = 0; k < nch && k < SEV_GPU_BRINGUP_MAX_CH; k++) {
					int rc = sev_gpu_do_submit_work(vm,
						bringup_ch[vm][k][0],
						bringup_ch[vm][k][1], true);

					pr_info("sev_gpu: bring-up GP_PUT ring VM%u hChannel=0x%x rc=%d\n",
						vm, bringup_ch[vm][k][1], rc);
					w->rings++;
				}
			}
		}

		/* Completion semaphore the client polls (0x3e pool + 0x3fffc). */
		o2 = READ_ONCE(osdesc_2m_off[vm]);
		if (o2 && o2 + 0x40000 <= (u64)dd->mem_size)
			sem = ioread32((u8 __iomem *)dd->mem + o2 + 0x3fffc);

		/*
		 * Broad diagnostic: the client's CUDA aborts on reading word
		 * 0x3fffc of its 0x3e control pool BEFORE it ever reaches the
		 * doorbell stage (no MAP_MEMORY of the usermode doorbell, no
		 * GPFIFO_SCHEDULE, +0x90 never written), so the +0x90 watch can't
		 * fire. The pool starts all-zero, so scan it for ANY non-zero
		 * dword: those are exactly the client's writes (USERD / GPFIFO /
		 * pushbuffer / GP_PUT it stages there). Their offsets reveal
		 * whether -- and where -- CUDA publishes work before bailing.
		 * Throttled; read-only.
		 */
		if (time_after_eq(jiffies, w->next_scan)) {
			u32 off, nz = 0, logged = 0;

			w->next_scan = jiffies +
				msecs_to_jiffies(SEV_GPU_BRINGUP_SCAN_MS);

			/*
			 * Scan the ENTIRE shadow usermode window (the full 64 KiB
			 * NV_VIRTUAL_FUNCTION region redirected from the *_USERMODE_A
			 * object -- clc761 BLACKWELL / clc661 HOPPER / clc361 VOLTA),
			 * not just the +0x90 doorbell word the ring-watch samples.
			 * The doorbell (NVC361_NOTIFY_CHANNEL_PENDING) is at window
			 * offset 0x90, in page 0, so it is covered either way; the
			 * wider sweep also catches any work-submit token the client
			 * writes elsewhere in the window. A populated 0x3e pushbuffer
			 * with rings=0 means CUDA staged work but never advanced the
			 * token. Logging every non-zero dword disambiguates "never
			 * rang" from "rang at an offset the +0x90 watch misses".
			 * Read-only.
			 */
			for (off = 0;
			     off + 4 <= SEV_GPU_COMPUTE_DOORBELL_PAGES * PAGE_SIZE &&
			     db + off + 4 <= (u64)dd->mem_size; off += 4) {
				u32 v = ioread32((u8 __iomem *)dd->mem + db + off);

				if (v)
					pr_info("sev_gpu: bring-up doorbell-scan VM%u db+0x%05x = 0x%08x\n",
						vm, off, v);
			}

			/*
			 * Sample the USERD ring pointers directly: GP_GET@0x88
			 * and GP_PUT@0x8C (NV_RAMUSERD_*). If GP_PUT has moved
			 * (esp. past GP_GET), CUDA queued a GPFIFO entry -- proof
			 * it published work regardless of whether the +0x90
			 * usermode doorbell ever fired (Hopper+/Blackwell CC
			 * channels kick via BAR0 0x30090, not the usermode page).
			 * userd_2m_off marks the first >=2 MiB carve, at whose head
			 * the replay-allocated USERD/GPFIFO lives. Read-only.
			 */
			{
				u64 u = READ_ONCE(userd_2m_off[vm]);

				if (u && u + SEV_GPU_USERD_GP_PUT_OFF + 4 <=
						(u64)dd->mem_size) {
					u32 gp_get = ioread32((u8 __iomem *)dd->mem +
							u + SEV_GPU_USERD_GP_GET_OFF);
					u32 gp_put = ioread32((u8 __iomem *)dd->mem +
							u + SEV_GPU_USERD_GP_PUT_OFF);

					if (gp_get || gp_put)
						pr_info("sev_gpu: bring-up USERD VM%u GP_GET@0x88=0x%08x GP_PUT@0x8C=0x%08x\n",
							vm, gp_get, gp_put);
				}
			}

			if (o2 && o2 + SEV_GPU_BRINGUP_SCAN_BYTES <=
					(u64)dd->mem_size) {
				for (off = 0; off < SEV_GPU_BRINGUP_SCAN_BYTES;
				     off += 4) {
					u32 v = ioread32((u8 __iomem *)dd->mem +
							 o2 + off);

					if (v) {
						nz++;
						if (logged < SEV_GPU_BRINGUP_SCAN_LOG) {
							pr_info("sev_gpu: bring-up scan VM%u 0x3e+0x%05x = 0x%08x\n",
								vm, off, v);
							logged++;
						}
					}
					if ((off & 0xffffu) == 0)
						cond_resched();
				}
				if (nz)
					pr_info("sev_gpu: bring-up scan VM%u total non-zero dwords=%u (of %u)\n",
						vm, nz,
						SEV_GPU_BRINGUP_SCAN_BYTES / 4);
			}
		}

		/*
		 * Report any advance of the 2nd-aperture (+0xf090) test word
		 * independently of +0x90 (which is dark), so this fires even when
		 * the primary doorbell never moves.
		 */
		if (tok_f != w->last_db_f) {
			pr_info("sev_gpu: bring-up watch VM%u doorbell +0xf090 0x%08x -> 0x%08x\n",
				vm, w->last_db_f, tok_f);
			w->last_db_f = tok_f;
		}

		if (tok == w->last_db)
			continue;	/* no advance since last sample */

		pr_info("sev_gpu: bring-up watch VM%u doorbell +0x90 0x%08x -> 0x%08x sem[0x3fffc]=0x%08x\n",
			vm, w->last_db, tok, sem);
		w->last_db = tok;

		if (tok != 0 && w->rings < SEV_GPU_BRINGUP_MAX_RINGS) {
			int rc = sev_gpu_do_submit_work(vm, w->h_client,
							w->h_channel, true);

			w->rings++;
			pr_info("sev_gpu: bring-up ring VM%u rc=%d (ring #%u)\n",
				vm, rc, w->rings);
		}
	}

	return any;
}

/*
 * Service one client mediated-copy request found in a data region's header in
 * the SEV_GPU_DATA_STAGED state. The manager drives the GPU on the client's
 * behalf (it is the sole doorbell-ringer), publishes the status and flips the
 * state to SEV_GPU_DATA_DONE. @vm_id is the manager's pool index for this
 * region -- the trusted identity, NOT the client-written owner_vm_id.
 */
static void sev_gpu_copy_service(struct sev_gpu_data_dev *dd, u32 vm_id)
{
	void __iomem *hdr = dd->mem;
	sev_gpu_data_header_t h;
	int rc;

	memcpy_fromio(&h, hdr, sizeof(h));
	if (h.magic != SEV_GPU_DATA_MAGIC || h.state != SEV_GPU_DATA_STAGED) {
		/* DIAG: surface why a kick did not turn into a serviced copy. */
		if (h.magic == SEV_GPU_DATA_MAGIC && h.state != SEV_GPU_DATA_FREE &&
		    h.state != SEV_GPU_DATA_BOUND && h.state != SEV_GPU_DATA_DONE)
			pr_info("sev_gpu: copy_service[%u]: magic=0x%llx state=%u (not STAGED=%u)\n",
				vm_id, (unsigned long long)h.magic, h.state,
				SEV_GPU_DATA_STAGED);
		return;
	}

	if (h.req_kind == SEV_GPU_REQ_KIND_SUBMIT_WORK) {
		pr_info("sev_gpu: copy_service[%u]: STAGED work-submit hClient=0x%x hChannel=0x%x\n",
			vm_id, h.req_h_client, h.req_h_channel);

		/* Client now drives submissions explicitly: stop the bring-up
		 * doorbell watcher so it can't double-ring the same channel. */
		sev_gpu_bringup_disarm(vm_id);

		/* Claim the job so a concurrent kick cannot double-service it. */
		iowrite32(SEV_GPU_DATA_INFLIGHT,
			  hdr + offsetof(sev_gpu_data_header_t, state));
		wmb();

		rc = sev_gpu_do_submit_work(vm_id, h.req_h_client,
					    h.req_h_channel, false);
		pr_info("sev_gpu: copy_service[%u]: submit_work rc=%d -> DONE\n",
			vm_id, rc);

		iowrite32((u32)rc,
			  hdr + offsetof(sev_gpu_data_header_t, req_status));
		wmb();
		iowrite32(SEV_GPU_DATA_DONE,
			  hdr + offsetof(sev_gpu_data_header_t, state));

		if (ctrl_dev && vm_id < SEV_GPU_MAX_VMS)
			ivshmem_ring(ctrl_dev, (u16)vm_id, IVSHMEM_VECTOR_RPC);
		return;
	}

	if (h.req_kind == SEV_GPU_REQ_KIND_FLUSH_ALL) {
		u32 h_clients[SEV_GPU_MAX_CHANNELS_PER_VM];
		u32 h_channels[SEV_GPU_MAX_CHANNELS_PER_VM];
		int i, count = 0, final_rc = 0;

		pr_info("sev_gpu: copy_service[%u]: STAGED flush-all compute channels\n",
			vm_id);

		/* Steady-state flushing begins: retire the bring-up watcher. */
		sev_gpu_bringup_disarm(vm_id);

		iowrite32(SEV_GPU_DATA_INFLIGHT,
			  hdr + offsetof(sev_gpu_data_header_t, state));
		wmb();

		/* Snapshot the assigned compute handles under the spinlock so we
		 * don't hold it across the GPU doorbell ring. */
		spin_lock(&assign_state.lock);
		for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
			struct sev_gpu_assignment *a = &assign_state.a[vm_id][i];

			if (!a->in_use || a->kind != SEV_GPU_CHAN_KIND_COMPUTE)
				continue;
			h_clients[count]  = a->h_client;
			h_channels[count] = a->h_channel;
			count++;
		}
		spin_unlock(&assign_state.lock);

		for (i = 0; i < count; i++) {
			rc = sev_gpu_do_submit_work(vm_id,
						    h_clients[i], h_channels[i],
						    false);
			if (rc && !final_rc)
				final_rc = rc;
		}
		pr_info("sev_gpu: copy_service[%u]: flushed %d compute channel(s) rc=%d\n",
			vm_id, count, final_rc);

		iowrite32((u32)final_rc,
			  hdr + offsetof(sev_gpu_data_header_t, req_status));
		wmb();
		iowrite32(SEV_GPU_DATA_DONE,
			  hdr + offsetof(sev_gpu_data_header_t, state));

		if (ctrl_dev && vm_id < SEV_GPU_MAX_VMS)
			ivshmem_ring(ctrl_dev, (u16)vm_id, IVSHMEM_VECTOR_RPC);
		return;
	}

	pr_info("sev_gpu: copy_service[%u]: STAGED ch=%u flags=0x%x len=%llu tag_off=%llu iv_off=%llu\n",
		vm_id, h.req_channel_id, h.req_flags,
		(unsigned long long)h.req_length,
		(unsigned long long)h.req_auth_tag_offset,
		(unsigned long long)h.req_iv_offset);

	/* Claim the job so a concurrent kick cannot double-service it. */
	iowrite32(SEV_GPU_DATA_INFLIGHT, hdr + offsetof(sev_gpu_data_header_t, state));
	wmb();

	rc = sev_gpu_do_ce_copy(vm_id, h.req_channel_id, h.req_flags,
				h.req_generation, h.req_src_offset,
				h.req_dst_offset, h.req_length,
				h.req_auth_tag_offset, h.req_iv_offset);
	pr_info("sev_gpu: copy_service[%u]: ce_copy rc=%d -> DONE\n", vm_id, rc);

	/* Publish the result, then flip to DONE so the client never sees DONE
	 * before the status lands. */
	iowrite32((u32)rc, hdr + offsetof(sev_gpu_data_header_t, req_status));
	wmb();
	iowrite32(SEV_GPU_DATA_DONE, hdr + offsetof(sev_gpu_data_header_t, state));

	/* NAPI-style: the client polls, but kick it so it wakes promptly. */
	if (ctrl_dev && vm_id < SEV_GPU_MAX_VMS)
		ivshmem_ring(ctrl_dev, (u16)vm_id, IVSHMEM_VECTOR_RPC);
}

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

static int sev_gpu_open(struct inode *inode, struct file *filp)
{
	filp->private_data = ctrl_dev;
	return 0;
}

static int sev_gpu_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * Map the shared CONTROL region (BAR2) into user space. This region holds only
 * scheduling metadata (header + request/grant rings); GPU-work payload lives in
 * the separate, hardware-isolated per-VM data devices, so there is nothing
 * sensitive to bound here.
 */
static int sev_gpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sev_gpu_dev *d = filp->private_data;
	unsigned long vsize = vma->vm_end - vma->vm_start;

	if (!d || !d->shmem_phys)
		return -ENODEV;
	if (vsize > d->shmem_size)
		return -EINVAL;

	/* BAR is shared (unencrypted) MMIO-backed RAM: map decrypted + uncached. */
	vma->vm_page_prot = pgprot_decrypted(pgprot_noncached(vma->vm_page_prot));

	return io_remap_pfn_range(vma, vma->vm_start,
				  d->shmem_phys >> PAGE_SHIFT,
				  vsize, vma->vm_page_prot);
}

/*
 * Client: issue one RM-RPC request through our own private mailbox and block
 * for the reply. Serialized so only one request is in flight at a time. The
 * blob travels inline in the mailbox slot; the manager (loopback) echoes it.
 */
static long sev_gpu_rpc_client_call(struct sev_gpu_dev *d, void __user *argp)
{
	struct sev_gpu_data_dev *dd = data_devs[0];	/* client: single region */
	sev_gpu_ioctl_rpc_test_t *t;
	sev_gpu_rpc_slot_t *slot;
	void __iomem *mb;
	unsigned long deadline;
	u32 copy_len, state;
	long ret = 0;

	if (d->is_manager)
		return -EPERM;			/* run on a client (manager=0) */
	if (!dd || !dd->mem)
		return -ENODEV;
	if (dd->mem_size < SEV_GPU_RPC_STAGING_OFF)
		return -ENOSPC;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!t || !slot) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(t, argp, sizeof(*t))) {
		ret = -EFAULT;
		goto out;
	}

	copy_len = min_t(u32, t->size, SEV_GPU_RPC_INLINE_MAX);

	slot->magic        = SEV_GPU_RPC_MAGIC;
	slot->version      = SEV_GPU_RPC_VERSION;
	slot->client_vm_id = d->client_vm_id;
	slot->seq          = (u32)atomic_inc_return(&d->rpc_seq);
	slot->cmd          = t->cmd;
	slot->arg_size     = copy_len;
	slot->n_buffers    = 0;
	slot->rm_status    = 0;
	slot->ret          = 0;
	slot->state        = SEV_GPU_RPC_IDLE;	/* not REQUEST yet */
	memcpy(slot->inline_arg, t->data, copy_len);

	mb = dd->mem + SEV_GPU_RPC_MAILBOX_OFF;

	mutex_lock(&d->rpc_lock);

	/*
	 * Publish the whole slot with state still IDLE, then flip it to REQUEST
	 * last so the manager never observes REQUEST before the payload lands.
	 */
	memcpy_toio(mb, slot, sizeof(*slot));
	wmb();
	iowrite32(SEV_GPU_RPC_REQUEST, mb + offsetof(sev_gpu_rpc_slot_t, state));

	ivshmem_ring(d, sev_gpu_manager_peer(d), IVSHMEM_VECTOR_RPC);

	/* Poll for the reply (manager flips state to REPLY). */
	deadline = jiffies + msecs_to_jiffies(5000);
	for (;;) {
		state = ioread32(mb + offsetof(sev_gpu_rpc_slot_t, state));
		if (state == SEV_GPU_RPC_REPLY)
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
		memcpy_fromio(slot, mb, sizeof(*slot));
		t->rm_status = slot->rm_status;
		memcpy(t->data, slot->inline_arg, copy_len);
		/* Mark the mailbox idle again for the next request. */
		iowrite32(SEV_GPU_RPC_IDLE,
			  mb + offsetof(sev_gpu_rpc_slot_t, state));
	}

	mutex_unlock(&d->rpc_lock);

	if (ret == 0 && copy_to_user(argp, t, sizeof(*t)))
		ret = -EFAULT;

out:
	kfree(slot);
	kfree(t);
	return ret;
}

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
static inline void __iomem *kmb_mailbox(u8 vm)
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
static int sev_gpu_aead(bool enc,
			const u8 key[SEV_GPU_COMM_KEY_LEN],
			const u8 nonce[SEV_GPU_KMB_NONCE_LEN],
			const void *aad, unsigned int aad_len,
			void *data, unsigned int data_len,
			u8 tag[SEV_GPU_KMB_TAG_LEN])
{
	struct crypto_aead *tfm;
	struct aead_request *req;
	struct scatterlist sg;
	DECLARE_CRYPTO_WAIT(wait);
	unsigned int buf_len;
	u8 *buf;
	int ret;

	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	ret = crypto_aead_setkey(tfm, key, SEV_GPU_COMM_KEY_LEN);
	if (ret)
		goto out_tfm;
	ret = crypto_aead_setauthsize(tfm, SEV_GPU_KMB_TAG_LEN);
	if (ret)
		goto out_tfm;

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto out_tfm;
	}

	/* One contiguous buffer holding [aad][data][tag]. */
	buf_len = aad_len + data_len + SEV_GPU_KMB_TAG_LEN;
	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_req;
	}
	if (aad_len)
		memcpy(buf, aad, aad_len);
	memcpy(buf + aad_len, data, data_len);
	if (!enc)
		memcpy(buf + aad_len + data_len, tag, SEV_GPU_KMB_TAG_LEN);

	sg_init_one(&sg, buf, buf_len);
	aead_request_set_callback(req,
				  CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				  crypto_req_done, &wait);
	aead_request_set_ad(req, aad_len);

	if (enc) {
		aead_request_set_crypt(req, &sg, &sg, data_len, (u8 *)nonce);
		ret = crypto_wait_req(crypto_aead_encrypt(req), &wait);
		if (!ret) {
			memcpy(data, buf + aad_len, data_len);
			memcpy(tag, buf + aad_len + data_len, SEV_GPU_KMB_TAG_LEN);
		}
	} else {
		aead_request_set_crypt(req, &sg, &sg,
				       data_len + SEV_GPU_KMB_TAG_LEN, (u8 *)nonce);
		ret = crypto_wait_req(crypto_aead_decrypt(req), &wait);
		if (!ret)
			memcpy(data, buf + aad_len, data_len);
	}

	memzero_explicit(buf, buf_len);
	kfree(buf);
out_req:
	aead_request_free(req);
out_tfm:
	crypto_free_aead(tfm);
	return ret;
}

/* First 8 bytes of SHA-256(KMB): a stable fingerprint for the self-test. */
static int sev_gpu_kmb_fp(const struct sev_cc_kmb *kmb, u8 fp[8])
{
	struct crypto_shash *tfm;
	u8 digest[32];
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, (const u8 *)kmb,
					  sizeof(*kmb), digest);
		shash_desc_zero(desc);
	}
	if (!ret)
		memcpy(fp, digest, 8);
	memzero_explicit(digest, sizeof(digest));
	crypto_free_shash(tfm);
	return ret;
}

/*
 * Manager: choose a channel for vm_id and stage its KMB into the assignment
 * registry. Pool mode (default) hands out a pre-provisioned channel of the
 * given keyspace; direct mode pins caller-supplied (manager-owned) handles;
 * with no GPU allocator a placeholder KMB keeps the seal/transport testable.
 * Fills *out_* (when non-NULL) with the manager's choice. The kmb_test
 * ASSIGN_CHANNEL ioctl and the automatic handshake worker share this path.
 */
static int sev_gpu_assign_channel(u8 vm_id, u32 keyspace,
				  u32 in_h_client, u32 in_h_channel,
				  u32 in_channel_id, u32 *out_channel_id,
				  u32 *out_h_client, u32 *out_h_channel)
{
	struct sev_gpu_assignment *slot = NULL;
	struct sev_gpu_cc_chan *pe = NULL;	/* reserved pool entry */
	struct sev_cc_kmb kmb;
	sev_gpu_kmb_fetch_t fetch;
	u32 h_client, h_channel, channel_id;
	bool real_kmb;
	int i;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;

	fetch = READ_ONCE(kmb_fetch_fn);

	if (in_h_client && in_h_channel) {
		if (!fetch)
			return -ENODEV;
		h_client   = in_h_client;
		h_channel  = in_h_channel;
		channel_id = in_channel_id;
		real_kmb   = true;
	} else if (READ_ONCE(chan_alloc_fn)) {
		spin_lock(&cc_pool.lock);
		for (i = 0; i < SEV_GPU_CC_POOL_MAX; i++) {
			struct sev_gpu_cc_chan *e = &cc_pool.e[i];

			if (e->provisioned && !e->in_use &&
			    e->keyspace == keyspace) {
				e->in_use   = true;
				e->owner_vm = vm_id;
				pe = e;
				break;
			}
		}
		spin_unlock(&cc_pool.lock);
		if (!pe)
			return -ENOSPC;	/* provision more of this keyspace */
		if (!fetch) {
			spin_lock(&cc_pool.lock);
			pe->in_use = false;
			spin_unlock(&cc_pool.lock);
			return -ENODEV;
		}
		h_client   = pe->h_client;
		h_channel  = pe->h_channel;
		channel_id = pe->channel_id;
		real_kmb   = true;
	} else {
		h_client = h_channel = 0;
		channel_id = in_channel_id;
		real_kmb = false;
	}

	/* Stage the key bundle outside the registry locks (may sleep). */
	if (real_kmb) {
		u32 st = fetch(h_client, h_channel, &kmb, sizeof(kmb));

		if (st != 0) {	/* 0 == NV_OK */
			pr_warn("sev_gpu: GET_KMB failed for ch %u (hClient 0x%x hChannel 0x%x) status 0x%x\n",
				channel_id, h_client, h_channel, st);
			memzero_explicit(&kmb, sizeof(kmb));
			if (pe) {
				spin_lock(&cc_pool.lock);
				pe->in_use = false;
				spin_unlock(&cc_pool.lock);
			}
			return -EIO;
		}
	} else {
		get_random_bytes(&kmb, sizeof(kmb));
	}

	spin_lock(&assign_state.lock);
	for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
		struct sev_gpu_assignment *e = &assign_state.a[vm_id][i];

		if (e->in_use && e->channel_id == channel_id) {
			slot = e;	/* re-assign existing channel */
			break;
		}
		if (!slot && !e->in_use)
			slot = e;	/* first free slot */
	}
	if (!slot) {
		spin_unlock(&assign_state.lock);
		memzero_explicit(&kmb, sizeof(kmb));
		if (pe) {
			spin_lock(&cc_pool.lock);
			pe->in_use = false;
			spin_unlock(&cc_pool.lock);
		}
		return -ENOSPC;
	}
	slot->in_use     = true;
	slot->kind       = SEV_GPU_CHAN_KIND_CE;
	slot->channel_id = channel_id;
	slot->keyspace   = keyspace;
	slot->h_client   = h_client;
	slot->h_channel  = h_channel;
	memcpy(&slot->kmb, &kmb, sizeof(slot->kmb));
	spin_unlock(&assign_state.lock);
	memzero_explicit(&kmb, sizeof(kmb));

	if (out_channel_id)
		*out_channel_id = channel_id;
	if (out_h_client)
		*out_h_client = h_client;
	if (out_h_channel)
		*out_h_channel = h_channel;

	pr_info("sev_gpu: assigned channel %u (keyspace %u) to VM%u [%s KMB]\n",
		channel_id, keyspace, vm_id,
		real_kmb ? (pe ? "pool" : "GPU") : "placeholder");
	return 0;
}

/*
 * Manager: assign a GR COMPUTE channel to vm_id (L3.3, allocate-on-assign per
 * Arch B "Option A"). Unlike the CE pool (pre-provisioned, keyspace-pooled), a
 * compute channel's USERD/GPFIFO must live in the ASSIGNEE's private DATA region
 * for zero-copy + per-client isolation, so the channel can only be built once we
 * know the client. The manager keeps a pool of SLOTS (the per-VM assignment
 * registry); on assign it carves that slot's USERD, GPFIFO, and pushbuffer
 * pages, asks nvidia.ko to build the channel backed by them (OS_PHYS_ADDR), and records the result. L4 then
 * fetches the channel's real CC_KMB (GET_KMB on the manager-owned handle, which
 * KernelChannel exports for any CC-secure GPFIFO channel) so SEND_KMB can seal +
 * deliver it; with no fetcher bound a placeholder keeps the transport testable.
 * Fills *out_* (when non-NULL) with the channel handles and the region-relative
 * USERD/GPFIFO offsets the client uses to publish work in place.
 */
static int sev_gpu_assign_compute_channel(u8 vm_id, u32 flags,
					  u32 *out_channel_id,
					  u32 *out_h_client, u32 *out_h_channel,
					  u64 *out_userd_off, u64 *out_gpfifo_off,
					  u64 *out_pushbuf_off, u64 *out_pushbuf_gpu_va)
{
	struct sev_gpu_assignment *slot = NULL;
	struct sev_gpu_data_dev *dd;
	sev_gpu_compute_alloc_t alloc;
	sev_gpu_compute_free_t  free_fn;
	struct sev_cc_kmb kmb;
	u64 userd_gpa = 0, gpfifo_gpa = 0, pushbuf_gpa = 0, enc_gpa = 0;
	u64 userd_off = 0, gpfifo_off = 0, pushbuf_off = 0, pushbuf_gpu_va = 0;
	u64 enc_off = 0;
	u32 h_client = 0, h_channel = 0, st;
	bool real_kmb;
	int idx = -1, i, ret;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;

	alloc   = READ_ONCE(compute_alloc_fn);
	free_fn = READ_ONCE(compute_free_fn);
	if (!alloc)
		return -ENODEV;	/* nvidia.ko compute provisioner not bound */

	/* The channel is backed by THIS client's private DATA region. */
	if (vm_id >= (u32)num_data_devs)
		return -ENXIO;
	dd = data_devs[vm_id];
	if (!dd || !dd->mem_phys)
		return -ENXIO;

	/* Reserve a free per-VM slot; its index fixes the carve location. */
	spin_lock(&assign_state.lock);
	for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
		if (!assign_state.a[vm_id][i].in_use) {
			slot = &assign_state.a[vm_id][i];
			idx = i;
			slot->in_use = true;	/* claim it before we drop the lock */
			break;
		}
	}
	spin_unlock(&assign_state.lock);
	if (!slot)
		return -ENOSPC;

	ret = sev_gpu_compute_carve(dd, (u32)idx, &userd_gpa, &gpfifo_gpa,
				    &pushbuf_gpa, &userd_off, &gpfifo_off,
				    &pushbuf_off, &enc_gpa, &enc_off);
	if (ret)
		goto release_slot;

	/*
	 * Zero the USERD/GPFIFO/PUSH pages here, through the manager's ioremap of
	 * this client's DATA region. RM's kfifoSetupUserD_GM107 memset of USERD
	 * runs into vmap() (mm/vmalloc.c:542) which CANNOT map an OS-descriptor
	 * over the ivshmem PCI BAR (no struct page, C-bit set) and fails with
	 * NV_ERR_INSUFFICIENT_RESOURCES, leaving USERD/GP_GET uninitialized so the
	 * channel runs on garbage. We own a working CPU mapping of the same shared
	 * pages, so clear them up front (USERD + GPFIFO ring + pushbuffer).
	 */
	if (dd->mem) {
		memset_io((u8 __iomem *)dd->mem + userd_off, 0, PAGE_SIZE);
		memset_io((u8 __iomem *)dd->mem + gpfifo_off, 0, PAGE_SIZE);
		memset_io((u8 __iomem *)dd->mem + pushbuf_off, 0, PAGE_SIZE);
	}

	/* Build the channel now, backed by the carved shared pages. */
	st = alloc(flags, userd_gpa, gpfifo_gpa, pushbuf_gpa,
		   &h_client, &h_channel, &pushbuf_gpu_va);
	if (st != 0) {	/* 0 == NV_OK */
		pr_warn("sev_gpu: compute-channel alloc failed for VM%u (USERD=0x%llx GPFIFO=0x%llx PUSH=0x%llx) status 0x%x\n",
			vm_id, userd_gpa, gpfifo_gpa, pushbuf_gpa, st);
		ret = -EIO;
		goto release_slot;
	}

	/*
	 * A compute (GR) channel is NOT individually CC-keyed: it executes
	 * inside the CPR (Compute Protected Region) and has no per-channel KMB.
	 * GET_KMB (NVC56F_CTRL_CMD_GET_KMB) returns NV_ERR_NOT_SUPPORTED (0x56)
	 * on any non-CC_SECURE channel, so it is deliberately NOT fetched here --
	 * doing so previously failed an otherwise-good channel build with -EIO.
	 * Client payload confidentiality is provided by the separate CE
	 * (copy-engine) channel's KMB (see the CC pool), which decrypts data into
	 * protected VRAM before the compute kernel runs. Record an inert
	 * placeholder so the slot and SEND_KMB path stay well-defined.
	 */
	get_random_bytes(&kmb, sizeof(kmb));
	real_kmb = false;

	spin_lock(&assign_state.lock);
	slot->kind       = SEV_GPU_CHAN_KIND_COMPUTE;
	slot->channel_id = h_channel;
	slot->keyspace   = 0;
	slot->h_client   = h_client;
	slot->h_channel  = h_channel;
	slot->userd_off  = userd_off;
	slot->gpfifo_off = gpfifo_off;
	slot->pushbuf_off = pushbuf_off;
	slot->pushbuf_gpu_va = pushbuf_gpu_va;
	slot->enc_off    = enc_off;
	memcpy(&slot->kmb, &kmb, sizeof(slot->kmb));
	spin_unlock(&assign_state.lock);
	memzero_explicit(&kmb, sizeof(kmb));

	if (out_channel_id)
		*out_channel_id = h_channel;
	if (out_h_client)
		*out_h_client = h_client;
	if (out_h_channel)
		*out_h_channel = h_channel;
	if (out_userd_off)
		*out_userd_off = userd_off;
	if (out_gpfifo_off)
		*out_gpfifo_off = gpfifo_off;
	if (out_pushbuf_off)
		*out_pushbuf_off = pushbuf_off;
	if (out_pushbuf_gpu_va)
		*out_pushbuf_gpu_va = pushbuf_gpu_va;

	pr_info("sev_gpu: assigned compute channel %u to VM%u [hClient=0x%x hChannel=0x%x USERD off=0x%llx GPFIFO off=0x%llx PUSH off=0x%llx pushVA=0x%llx, %s KMB]\n",
		h_channel, vm_id, h_client, h_channel, userd_off, gpfifo_off,
		pushbuf_off, pushbuf_gpu_va,
		real_kmb ? "GPU" : "placeholder");
	return 0;

release_slot:
	if (h_client && free_fn)
		free_fn(h_client);
	spin_lock(&assign_state.lock);
	memset(slot, 0, sizeof(*slot));
	spin_unlock(&assign_state.lock);
	return ret;
}

/*
 * Manager: seal the assigned channel's KMB under the comm key, post it to the
 * client's KMB mailbox, and block until the client acks (up to to_ms, 0 =
 * default 120s). fp_out[8] receives SHA256[:8] of the plaintext KMB on success.
 */
static int sev_gpu_send_kmb(u8 vm_id, u32 channel_id, unsigned int to_ms,
			    u8 fp_out[8])
{
	struct sev_gpu_kmb_aad aad;
	struct sev_cc_kmb kmb;
	struct sev_gpu_assignment *slot = NULL;
	void __iomem *mb;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce[SEV_GPU_KMB_NONCE_LEN];
	u8 tag[SEV_GPU_KMB_TAG_LEN];
	u8 fp[8];
	u32 keyspace = 0, seq;
	unsigned long deadline;
	bool have_key;
	int i, ret;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;

	mb = kmb_mailbox(vm_id);
	if (!mb)
		return -ENXIO;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm_id, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[vm_id], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key)
		return -ENOKEY;

	spin_lock(&assign_state.lock);
	for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
		struct sev_gpu_assignment *e = &assign_state.a[vm_id][i];

		if (e->in_use && e->channel_id == channel_id) {
			kmb = e->kmb;
			keyspace = e->keyspace;
			slot = e;
			break;
		}
	}
	spin_unlock(&assign_state.lock);
	if (!slot) {
		memzero_explicit(key, sizeof(key));
		return -ENOENT;	/* channel not assigned to this client */
	}

	ret = sev_gpu_kmb_fp(&kmb, fp);
	if (ret)
		goto send_out;

	get_random_bytes(nonce, sizeof(nonce));
	seq = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, seq)) + 1;

	/* Record this delivery's seq as the channel's KMB epoch (generation). */
	spin_lock(&assign_state.lock);
	slot->generation = seq;
	spin_unlock(&assign_state.lock);

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = vm_id;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Seal the KMB in place; kmb now holds ciphertext. */
	ret = sev_gpu_aead(true, key, nonce, &aad, sizeof(aad),
			   &kmb, sizeof(kmb), tag);
	if (ret)
		goto send_out;

	/* Publish ciphertext + metadata, then flip the slot to READY. */
	iowrite32(SEV_GPU_KMB_IDLE,
		  mb + offsetof(struct sev_gpu_kmb_mbox, state));
	iowrite32(SEV_GPU_KMB_MAGIC,
		  mb + offsetof(struct sev_gpu_kmb_mbox, magic));
	iowrite32(vm_id, mb + offsetof(struct sev_gpu_kmb_mbox, vm_id));
	iowrite32(channel_id,
		  mb + offsetof(struct sev_gpu_kmb_mbox, channel_id));
	iowrite32(keyspace,
		  mb + offsetof(struct sev_gpu_kmb_mbox, keyspace));
	iowrite32(seq, mb + offsetof(struct sev_gpu_kmb_mbox, seq));
	iowrite32((u32)sizeof(kmb),
		  mb + offsetof(struct sev_gpu_kmb_mbox, ct_len));
	memcpy_toio(mb + offsetof(struct sev_gpu_kmb_mbox, nonce),
		    nonce, sizeof(nonce));
	memcpy_toio(mb + offsetof(struct sev_gpu_kmb_mbox, tag),
		    tag, sizeof(tag));
	memcpy_toio(mb + offsetof(struct sev_gpu_kmb_mbox, ct),
		    &kmb, sizeof(kmb));
	wmb();
	iowrite32(SEV_GPU_KMB_READY,
		  mb + offsetof(struct sev_gpu_kmb_mbox, state));

	/* Wait for the client to consume + install it. */
	to_ms = to_ms ? to_ms : 120000;
	deadline = jiffies + msecs_to_jiffies(to_ms);
	ret = -ETIMEDOUT;
	while (time_before(jiffies, deadline)) {
		if (ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, state)) ==
		    SEV_GPU_KMB_ACK) {
			ret = 0;
			break;
		}
		if (msleep_interruptible(20)) {
			ret = -EINTR;
			break;
		}
	}

	if (ret == 0 && fp_out)
		memcpy(fp_out, fp, 8);

send_out:
	memzero_explicit(&kmb, sizeof(kmb));
	memzero_explicit(key, sizeof(key));
	return ret;
}

/*
 * Client: block until the manager posts a sealed KMB (up to to_ms, 0 = default
 * 120s), unseal it under the comm key, and install it into the per-channel
 * keystore. Fills out_channel_id/out_keyspace/fp_out[8] (when non-NULL).
 */
static int sev_gpu_recv_kmb(struct sev_gpu_dev *d, unsigned int to_ms,
			    u32 *out_channel_id, u32 *out_keyspace, u8 fp_out[8])
{
	struct sev_gpu_kmb_aad aad;
	struct sev_cc_kmb kmb;
	void __iomem *mb;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce[SEV_GPU_KMB_NONCE_LEN];
	u8 tag[SEV_GPU_KMB_TAG_LEN];
	u8 fp[8];
	u32 ct_len, channel_id, keyspace, seq, vm = d->comm_vm_id;
	unsigned long deadline;
	bool have_key;
	int ret;

	mb = kmb_mailbox(vm);
	if (!mb)
		return -ENXIO;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[vm], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key)
		return -ENOKEY;

	/* Wait for the manager to post a sealed KMB. */
	to_ms = to_ms ? to_ms : 120000;
	deadline = jiffies + msecs_to_jiffies(to_ms);
	ret = -ETIMEDOUT;
	while (time_before(jiffies, deadline)) {
		if (ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, state)) ==
		    SEV_GPU_KMB_READY) {
			ret = 0;
			break;
		}
		if (msleep_interruptible(20)) {
			ret = -EINTR;
			break;
		}
	}
	if (ret) {
		memzero_explicit(key, sizeof(key));
		return ret;
	}

	rmb();
	ct_len     = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, ct_len));
	channel_id = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, channel_id));
	keyspace   = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, keyspace));
	seq        = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, seq));
	if (ct_len != sizeof(kmb)) {
		ret = -EPROTO;
		goto recv_out;
	}
	memcpy_fromio(nonce, mb + offsetof(struct sev_gpu_kmb_mbox, nonce),
		      sizeof(nonce));
	memcpy_fromio(tag, mb + offsetof(struct sev_gpu_kmb_mbox, tag),
		      sizeof(tag));
	memcpy_fromio(&kmb, mb + offsetof(struct sev_gpu_kmb_mbox, ct),
		      sizeof(kmb));

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = vm;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Unseal in place; auth failure (tampered ciphertext) => -EBADMSG. */
	ret = sev_gpu_aead(false, key, nonce, &aad, sizeof(aad),
			   &kmb, sizeof(kmb), tag);
	if (ret)
		goto recv_out;

	ret = sev_gpu_kmb_fp(&kmb, fp);
	if (ret)
		goto recv_out;

	/* Install the unsealed KMB into the client's per-channel keystore. */
	{
		int slot = -1, j;

		spin_lock(&client_kmb_store.lock);
		for (j = 0; j < SEV_GPU_MAX_CHANNELS_PER_VM; j++) {
			struct sev_client_chan *e = &client_kmb_store.c[j];

			if (e->valid && e->channel_id == channel_id) {
				slot = j;
				break;
			}
			if (slot < 0 && !e->valid)
				slot = j;
		}
		if (slot >= 0) {
			struct sev_client_chan *e = &client_kmb_store.c[slot];

			e->kmb        = kmb;
			e->channel_id = channel_id;
			e->keyspace   = keyspace;
			e->generation = seq;	/* KMB epoch */
			e->ctr_h2d    = 0;	/* fresh key => reset IV */
			e->ctr_d2h    = 0;
			e->valid      = true;
		}
		spin_unlock(&client_kmb_store.lock);
		if (slot < 0) {
			ret = -ENOSPC;	/* no free channel slot */
			goto recv_out;
		}
	}

	if (out_channel_id)
		*out_channel_id = channel_id;
	if (out_keyspace)
		*out_keyspace = keyspace;
	if (fp_out)
		memcpy(fp_out, fp, 8);

	/* Ack so the manager unblocks. */
	wmb();
	iowrite32(SEV_GPU_KMB_ACK,
		  mb + offsetof(struct sev_gpu_kmb_mbox, state));

	pr_info("sev_gpu: recv KMB ch %u fp %02x%02x%02x%02x%02x%02x%02x%02x\n",
		channel_id, fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6],
		fp[7]);

recv_out:
	memzero_explicit(&kmb, sizeof(kmb));
	memzero_explicit(key, sizeof(key));
	return ret;
}

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

#define SEV_GPU_HS_MAGIC	0x31534b48u	/* "HKS1" */
#define SEV_GPU_HS_VERSION	1u

/* Mailbox state word. Distinct 32-bit tags so a zeroed/garbage BAR never
 * looks like a live request. */
#define SEV_GPU_HS_IDLE		0u
#define SEV_GPU_HS_REQ		0x48534b31u	/* client posted a request */
#define SEV_GPU_HS_REPLY	0x48534b32u	/* manager posted the reply */

/* Client -> manager message types. */
#define SEV_GPU_HS_MSG_HELLO	1u	/* {pub_c, nonce_c}   */
#define SEV_GPU_HS_MSG_FINISHED	2u	/* {confirm_c}        */

#define SEV_GPU_HS_CURVE	"ecdh-nist-p256"
#define SEV_GPU_HS_PUBKEY_LEN	64	/* P-256 uncompressed x||y */
#define SEV_GPU_HS_SECRET_LEN	32	/* P-256 ECDH shared x     */
#define SEV_GPU_HS_NONCE_LEN	32
#define SEV_GPU_HS_CONFIRM_LEN	32	/* HMAC-SHA256 output      */
#define SEV_GPU_HS_MAX_ATTEMPTS	8

/* Per-VM handshake mailbox, laid out at the start of each VM's slot in the
 * (retired keybroker) TLS ring region. Written/read with memcpy_*io. */
typedef struct {
	__u32 magic;
	__u32 version;
	__u32 state;		/* SEV_GPU_HS_IDLE/REQ/REPLY */
	__u32 msg_type;		/* SEV_GPU_HS_MSG_* (client -> manager) */
	__u32 status;		/* manager reply: 0 == ok */
	__u32 reserved;
	__u8  pub_c[SEV_GPU_HS_PUBKEY_LEN];
	__u8  nonce_c[SEV_GPU_HS_NONCE_LEN];
	__u8  pub_s[SEV_GPU_HS_PUBKEY_LEN];
	__u8  nonce_s[SEV_GPU_HS_NONCE_LEN];
	__u8  confirm_s[SEV_GPU_HS_CONFIRM_LEN];
	__u8  confirm_c[SEV_GPU_HS_CONFIRM_LEN];
} sev_gpu_hs_slot_t;

/* Manager per-VM state carried between HELLO and FINISHED. Accessed only from
 * the single manager poller kthread, so no lock is needed here; the comm key
 * commit itself takes comm_keystore.lock. */
static struct {
	bool active;
	u8   comm_key[SEV_GPU_COMM_KEY_LEN];
	u8   confirm_key[SEV_GPU_HS_CONFIRM_LEN];
	u8   th[32];
} hs_mgr_state[SEV_GPU_MAX_VMS];

/* Client-side per-VM run guards (multiple CUDA threads may forward at once). */
static atomic_t hs_client_busy[SEV_GPU_MAX_VMS];
static atomic_t hs_client_attempts[SEV_GPU_MAX_VMS];

/* Cached shared PSK (loaded lazily from psk_path on first handshake). */
static u8 sev_gpu_psk[SEV_GPU_COMM_KEY_LEN];
static bool sev_gpu_psk_valid;
static DEFINE_MUTEX(sev_gpu_psk_lock);

/* Handshake mailbox for VM @vm: start of its slot in the TLS ring region. */
static void __iomem *hs_ctrl_mailbox(u8 vm)
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
static int sev_gpu_get_psk(u8 out[SEV_GPU_COMM_KEY_LEN])
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

/* SHA-256 of a single contiguous buffer. */
static int sev_gpu_sha256(const u8 *data, unsigned int dlen, u8 out[32])
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, data, dlen, out);
		shash_desc_zero(desc);
	}
	crypto_free_shash(tfm);
	return ret;
}

/* HMAC-SHA256(key, data) -> out[32]. */
static int sev_gpu_hmac_sha256(const u8 *key, unsigned int klen,
			       const u8 *data, unsigned int dlen, u8 out[32])
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	ret = crypto_shash_setkey(tfm, key, klen);
	if (!ret) {
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, data, dlen, out);
		shash_desc_zero(desc);
	}
	crypto_free_shash(tfm);
	return ret;
}

/* HKDF-Expand (single 32-byte block): T = HMAC(prk, label || th || 0x01). */
static int sev_gpu_hkdf_expand32(const u8 prk[32], const char *label,
				 const u8 th[32], u8 out[32])
{
	u8 info[64 + 32 + 1];
	unsigned int llen = (unsigned int)strlen(label);
	unsigned int n = 0;

	if (llen > 64)
		return -EINVAL;
	memcpy(info, label, llen);
	n += llen;
	memcpy(info + n, th, 32);
	n += 32;
	info[n++] = 0x01;
	return sev_gpu_hmac_sha256(prk, 32, info, n, out);
}

/*
 * Shared key schedule (identical on both sides):
 *   transcript = pub_c || nonce_c || pub_s || nonce_s
 *   th         = SHA256(transcript)
 *   PRK        = HKDF-Extract(salt = PSK, IKM = Z)          [binds PSK + ECDH]
 *   comm_key   = HKDF-Expand(PRK, "sev-gpu comm v1"    || th)
 *   confirm_key= HKDF-Expand(PRK, "sev-gpu confirm v1" || th)
 */
static int sev_gpu_hs_derive(const u8 *pub_c, const u8 *nonce_c,
			     const u8 *pub_s, const u8 *nonce_s,
			     const u8 *z, const u8 *psk,
			     u8 comm_key[32], u8 confirm_key[32], u8 th_out[32])
{
	u8 transcript[2 * (SEV_GPU_HS_PUBKEY_LEN + SEV_GPU_HS_NONCE_LEN)];
	u8 th[32], prk[32];
	int ret;

	memcpy(transcript, pub_c, SEV_GPU_HS_PUBKEY_LEN);
	memcpy(transcript + 64, nonce_c, SEV_GPU_HS_NONCE_LEN);
	memcpy(transcript + 96, pub_s, SEV_GPU_HS_PUBKEY_LEN);
	memcpy(transcript + 160, nonce_s, SEV_GPU_HS_NONCE_LEN);

	ret = sev_gpu_sha256(transcript, sizeof(transcript), th);
	if (ret)
		goto out;
	/* HKDF-Extract(salt=PSK, IKM=Z) */
	ret = sev_gpu_hmac_sha256(psk, SEV_GPU_COMM_KEY_LEN, z,
				  SEV_GPU_HS_SECRET_LEN, prk);
	if (ret)
		goto out;
	ret = sev_gpu_hkdf_expand32(prk, "sev-gpu comm v1", th, comm_key);
	if (ret)
		goto out;
	ret = sev_gpu_hkdf_expand32(prk, "sev-gpu confirm v1", th, confirm_key);
	if (ret)
		goto out;
	memcpy(th_out, th, 32);
out:
	memzero_explicit(prk, sizeof(prk));
	memzero_explicit(transcript, sizeof(transcript));
	return ret;
}

/* Ephemeral P-256 ECDH context (holds the private key inside the kpp tfm). */
struct sev_gpu_ecdhe {
	struct crypto_kpp *tfm;
	u8 pub[SEV_GPU_HS_PUBKEY_LEN];
};

/* Allocate an ephemeral keypair and export the public key into e->pub. */
static int sev_gpu_ecdhe_init(struct sev_gpu_ecdhe *e)
{
	struct ecdh params = { .key = NULL, .key_size = 0 };
	struct kpp_request *req = NULL;
	struct scatterlist dst;
	DECLARE_CRYPTO_WAIT(wait);
	char *encoded = NULL;
	u8 *pubbuf = NULL;		/* heap: scatterlists must not sit on a
					 * VMAP_STACK stack buffer */
	unsigned int elen;
	int ret;

	e->tfm = crypto_alloc_kpp(SEV_GPU_HS_CURVE, 0, 0);
	if (IS_ERR(e->tfm)) {
		ret = PTR_ERR(e->tfm);
		e->tfm = NULL;
		return ret;
	}
	elen = crypto_ecdh_key_len(&params);
	encoded = kmalloc(elen, GFP_KERNEL);
	pubbuf = kmalloc(SEV_GPU_HS_PUBKEY_LEN, GFP_KERNEL);
	if (!encoded || !pubbuf) {
		ret = -ENOMEM;
		goto err;
	}
	/* key=NULL/key_size=0 -> kernel generates a random ephemeral privkey. */
	ret = crypto_ecdh_encode_key(encoded, elen, &params);
	if (ret)
		goto err;
	ret = crypto_kpp_set_secret(e->tfm, encoded, elen);
	if (ret)
		goto err;
	req = kpp_request_alloc(e->tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto err;
	}
	sg_init_one(&dst, pubbuf, SEV_GPU_HS_PUBKEY_LEN);
	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dst, SEV_GPU_HS_PUBKEY_LEN);
	kpp_request_set_callback(req,
				 CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				 crypto_req_done, &wait);
	ret = crypto_wait_req(crypto_kpp_generate_public_key(req), &wait);
	kpp_request_free(req);
	if (ret)
		goto err;
	memcpy(e->pub, pubbuf, SEV_GPU_HS_PUBKEY_LEN);
	kfree_sensitive(encoded);
	kfree_sensitive(pubbuf);
	return 0;
err:
	kfree_sensitive(encoded);
	kfree_sensitive(pubbuf);
	if (e->tfm) {
		crypto_free_kpp(e->tfm);
		e->tfm = NULL;
	}
	return ret;
}

/* Compute the ECDH shared secret with @peer_pub -> out[32]. */
static int sev_gpu_ecdhe_shared(struct sev_gpu_ecdhe *e,
				const u8 peer_pub[SEV_GPU_HS_PUBKEY_LEN],
				u8 out[SEV_GPU_HS_SECRET_LEN])
{
	struct kpp_request *req;
	struct scatterlist src, dst;
	DECLARE_CRYPTO_WAIT(wait);
	u8 *inbuf, *outbuf;		/* heap: see sev_gpu_ecdhe_init */
	int ret;

	if (!e->tfm)
		return -EINVAL;
	inbuf = kmalloc(SEV_GPU_HS_PUBKEY_LEN, GFP_KERNEL);
	outbuf = kmalloc(SEV_GPU_HS_SECRET_LEN, GFP_KERNEL);
	if (!inbuf || !outbuf) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(inbuf, peer_pub, SEV_GPU_HS_PUBKEY_LEN);
	req = kpp_request_alloc(e->tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto out;
	}
	sg_init_one(&src, inbuf, SEV_GPU_HS_PUBKEY_LEN);
	sg_init_one(&dst, outbuf, SEV_GPU_HS_SECRET_LEN);
	kpp_request_set_input(req, &src, SEV_GPU_HS_PUBKEY_LEN);
	kpp_request_set_output(req, &dst, SEV_GPU_HS_SECRET_LEN);
	kpp_request_set_callback(req,
				 CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				 crypto_req_done, &wait);
	ret = crypto_wait_req(crypto_kpp_compute_shared_secret(req), &wait);
	kpp_request_free(req);
	if (!ret)
		memcpy(out, outbuf, SEV_GPU_HS_SECRET_LEN);
out:
	kfree_sensitive(inbuf);
	kfree_sensitive(outbuf);
	return ret;
}

static void sev_gpu_ecdhe_free(struct sev_gpu_ecdhe *e)
{
	if (e->tfm) {
		crypto_free_kpp(e->tfm);
		e->tfm = NULL;
	}
	memzero_explicit(e->pub, sizeof(e->pub));
}

/* Commit an established comm key into comm_keystore (shared by SET_COMM_KEY
 * and the in-kernel handshake). */
static void sev_gpu_commit_comm_key(u32 vm, const u8 key[SEV_GPU_COMM_KEY_LEN])
{
	spin_lock(&comm_keystore.lock);
	memcpy(comm_keystore.key[vm], key, SEV_GPU_COMM_KEY_LEN);
	set_bit(vm, &comm_keystore.valid);
	spin_unlock(&comm_keystore.lock);

	/* Comm channel + KMB now established for this client: this is the gate for
	 * building the manager's per-client UVM channels (queued to a workqueue, so
	 * the heavy retain never runs under comm_keystore.lock or on the poller). */
	sev_gpu_manager_note_client_active(vm);
}

/* Bounded wait for the comm key of @vm to appear (handshake completing on
 * another thread / the peer). Kept below RPC_TIMEOUT_MS by the caller. */
static bool sev_gpu_wait_comm_key(u32 vm, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	if (vm >= SEV_GPU_MAX_VMS)
		return false;
	for (;;) {
		bool ok;

		spin_lock(&comm_keystore.lock);
		ok = test_bit(vm, &comm_keystore.valid);
		spin_unlock(&comm_keystore.lock);
		if (ok)
			return true;
		if (time_after(jiffies, deadline) || signal_pending(current))
			return false;
		msleep(20);
	}
}

/*
 * Manager: service one handshake mailbox found in the REQ state. Runs in the
 * single manager poller kthread context (see rpc_thread_fn). HELLO derives the
 * shared secret + keys and returns pub_s/nonce_s/confirm_s; FINISHED verifies
 * the client's confirm and, on success, commits the comm key.
 */
static void sev_gpu_hs_service_slot(u8 vm, void __iomem *hb)
{
	sev_gpu_hs_slot_t s;
	struct sev_gpu_ecdhe e = { .tfm = NULL };
	u8 psk[SEV_GPU_COMM_KEY_LEN];
	u8 z[SEV_GPU_HS_SECRET_LEN];
	u8 comm_key[32], confirm_key[32], th[32], mac[32];
	u32 status = 1;
	int ret;

	memcpy_fromio(&s, hb, sizeof(s));
	if (s.magic != SEV_GPU_HS_MAGIC || s.version != SEV_GPU_HS_VERSION) {
		/* stale/garbage: drop back to idle so we don't spin on it */
		iowrite32(SEV_GPU_HS_IDLE,
			  hb + offsetof(sev_gpu_hs_slot_t, state));
		return;
	}
	if (sev_gpu_get_psk(psk))
		goto reply;

	if (s.msg_type == SEV_GPU_HS_MSG_HELLO) {
		ret = sev_gpu_ecdhe_init(&e);
		if (ret) {
			pr_warn("sev_gpu: auto-mTLS: mgr ecdhe init %d\n", ret);
			goto reply;
		}
		memcpy(s.pub_s, e.pub, sizeof(s.pub_s));
		get_random_bytes(s.nonce_s, sizeof(s.nonce_s));
		ret = sev_gpu_ecdhe_shared(&e, s.pub_c, z);
		if (ret) {
			pr_warn("sev_gpu: auto-mTLS: mgr ecdh shared %d\n", ret);
			goto reply;
		}
		ret = sev_gpu_hs_derive(s.pub_c, s.nonce_c, s.pub_s, s.nonce_s,
					z, psk, comm_key, confirm_key, th);
		if (ret)
			goto reply;
		ret = sev_gpu_hmac_sha256(confirm_key, sizeof(confirm_key),
					  (const u8 *)"s", 1, mac);
		if (ret)
			goto reply;
		memcpy(s.confirm_s, mac, sizeof(s.confirm_s));
		memcpy(hs_mgr_state[vm].comm_key, comm_key, sizeof(comm_key));
		memcpy(hs_mgr_state[vm].confirm_key, confirm_key,
		       sizeof(confirm_key));
		memcpy(hs_mgr_state[vm].th, th, sizeof(th));
		hs_mgr_state[vm].active = true;
		status = 0;
		pr_info("sev_gpu: auto-mTLS: mgr HELLO ok for vm %u\n", vm);
	} else if (s.msg_type == SEV_GPU_HS_MSG_FINISHED) {
		if (!hs_mgr_state[vm].active) {
			pr_warn("sev_gpu: auto-mTLS: FINISHED without HELLO vm %u\n",
				vm);
			goto reply;
		}
		ret = sev_gpu_hmac_sha256(hs_mgr_state[vm].confirm_key,
					  SEV_GPU_HS_CONFIRM_LEN,
					  (const u8 *)"c", 1, mac);
		if (ret)
			goto clear;
		if (crypto_memneq(mac, s.confirm_c, sizeof(mac))) {
			pr_warn("sev_gpu: auto-mTLS: client confirm mismatch vm %u (MITM/PSK?)\n",
				vm);
			goto clear;
		}
		sev_gpu_commit_comm_key(vm, hs_mgr_state[vm].comm_key);
		status = 0;
		pr_info("sev_gpu: auto-mTLS: mgr FINISHED ok, comm key committed for vm %u\n",
			vm);
clear:
		memzero_explicit(&hs_mgr_state[vm], sizeof(hs_mgr_state[vm]));
	} else {
		pr_warn("sev_gpu: auto-mTLS: unknown msg_type %u vm %u\n",
			s.msg_type, vm);
	}

reply:
	s.status = status;
	s.state = SEV_GPU_HS_IDLE;		/* publish payload before state */
	memcpy_toio(hb, &s, sizeof(s));
	wmb();
	iowrite32(SEV_GPU_HS_REPLY, hb + offsetof(sev_gpu_hs_slot_t, state));
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, (u16)vm, IVSHMEM_VECTOR_RPC);

	sev_gpu_ecdhe_free(&e);
	memzero_explicit(psk, sizeof(psk));
	memzero_explicit(z, sizeof(z));
	memzero_explicit(comm_key, sizeof(comm_key));
	memzero_explicit(confirm_key, sizeof(confirm_key));
}

/* Client: poll the handshake mailbox until it flips to REPLY (bounded). */
static int sev_gpu_hs_wait_reply(void __iomem *hb)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(auto_mtls_wait_ms);

	for (;;) {
		if (ioread32(hb + offsetof(sev_gpu_hs_slot_t, state)) ==
		    SEV_GPU_HS_REPLY)
			return 0;
		if (time_after(jiffies, deadline) || signal_pending(current))
			return -ETIMEDOUT;
		usleep_range(200, 500);
	}
}

/*
 * Client: drive the full ECDHE-PSK handshake for @vm inline (blocking, ~2 RTT
 * over shared memory). On success installs the derived comm key and returns 0.
 */
static int sev_gpu_hs_client_run(u32 vm)
{
	void __iomem *hb = hs_ctrl_mailbox((u8)vm);
	struct sev_gpu_ecdhe e = { .tfm = NULL };
	sev_gpu_hs_slot_t s;
	u8 psk[SEV_GPU_COMM_KEY_LEN];
	u8 z[SEV_GPU_HS_SECRET_LEN];
	u8 my_nonce_c[SEV_GPU_HS_NONCE_LEN];
	u8 comm_key[32], confirm_key[32], th[32], mac[32];
	int ret;

	if (!hb)
		return -ENODEV;
	ret = sev_gpu_get_psk(psk);
	if (ret)
		return ret;
	ret = sev_gpu_ecdhe_init(&e);
	if (ret) {
		memzero_explicit(psk, sizeof(psk));
		return ret;
	}

	/* ---- Phase 1: ClientHello {pub_c, nonce_c} ---- */
	get_random_bytes(my_nonce_c, sizeof(my_nonce_c));
	memset(&s, 0, sizeof(s));
	s.magic = SEV_GPU_HS_MAGIC;
	s.version = SEV_GPU_HS_VERSION;
	s.msg_type = SEV_GPU_HS_MSG_HELLO;
	memcpy(s.pub_c, e.pub, sizeof(s.pub_c));
	memcpy(s.nonce_c, my_nonce_c, sizeof(s.nonce_c));
	s.state = SEV_GPU_HS_IDLE;
	memcpy_toio(hb, &s, sizeof(s));
	wmb();
	iowrite32(SEV_GPU_HS_REQ, hb + offsetof(sev_gpu_hs_slot_t, state));
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, sev_gpu_manager_peer(ctrl_dev),
			     IVSHMEM_VECTOR_RPC);

	ret = sev_gpu_hs_wait_reply(hb);
	if (ret)
		goto out;
	memcpy_fromio(&s, hb, sizeof(s));
	if (s.status != 0) {
		pr_warn("sev_gpu: auto-mTLS: mgr rejected HELLO (status %u) vm %u\n",
			s.status, vm);
		ret = -EPROTO;
		goto out;
	}

	/* Derive using our own local pub_c/nonce_c (tamper -> key divergence). */
	ret = sev_gpu_ecdhe_shared(&e, s.pub_s, z);
	if (ret)
		goto out;
	ret = sev_gpu_hs_derive(e.pub, my_nonce_c, s.pub_s, s.nonce_s,
				z, psk, comm_key, confirm_key, th);
	if (ret)
		goto out;
	ret = sev_gpu_hmac_sha256(confirm_key, sizeof(confirm_key),
				  (const u8 *)"s", 1, mac);
	if (ret)
		goto out;
	if (crypto_memneq(mac, s.confirm_s, sizeof(mac))) {
		pr_warn("sev_gpu: auto-mTLS: server confirm mismatch vm %u (MITM/PSK?)\n",
			vm);
		ret = -EKEYREJECTED;
		goto out;
	}

	/* ---- Phase 2: Finished {confirm_c} ---- */
	ret = sev_gpu_hmac_sha256(confirm_key, sizeof(confirm_key),
				  (const u8 *)"c", 1, mac);
	if (ret)
		goto out;
	memset(&s, 0, sizeof(s));
	s.magic = SEV_GPU_HS_MAGIC;
	s.version = SEV_GPU_HS_VERSION;
	s.msg_type = SEV_GPU_HS_MSG_FINISHED;
	memcpy(s.confirm_c, mac, sizeof(s.confirm_c));
	s.state = SEV_GPU_HS_IDLE;
	memcpy_toio(hb, &s, sizeof(s));
	wmb();
	iowrite32(SEV_GPU_HS_REQ, hb + offsetof(sev_gpu_hs_slot_t, state));
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, sev_gpu_manager_peer(ctrl_dev),
			     IVSHMEM_VECTOR_RPC);

	ret = sev_gpu_hs_wait_reply(hb);
	if (ret)
		goto out;
	memcpy_fromio(&s, hb, sizeof(s));
	if (s.status != 0) {
		pr_warn("sev_gpu: auto-mTLS: mgr rejected FINISHED (status %u) vm %u\n",
			s.status, vm);
		ret = -EPROTO;
		goto out;
	}

	/* Both sides confirmed: install the comm key. */
	if (ctrl_dev && !ctrl_dev->is_manager)
		ctrl_dev->comm_vm_id = (u8)vm;
	sev_gpu_commit_comm_key(vm, comm_key);
	pr_info("sev_gpu: auto-mTLS: client handshake complete, comm key installed for vm %u\n",
		vm);
	ret = 0;
out:
	sev_gpu_ecdhe_free(&e);
	memzero_explicit(psk, sizeof(psk));
	memzero_explicit(z, sizeof(z));
	memzero_explicit(comm_key, sizeof(comm_key));
	memzero_explicit(confirm_key, sizeof(confirm_key));
	return ret;
}

/*
 * Client: opportunistically run the in-kernel handshake for @vm on first
 * contact, if enabled and no comm key exists yet. One runner at a time; other
 * threads proceed (their sealed-KMB path waits for the key via
 * sev_gpu_wait_comm_key). Bounded retries guard against a not-yet-ready peer.
 */
static void sev_gpu_hs_client_maybe_run(u8 vm)
{
	bool have_key;
	int ret;

	if (!auto_mtls || vm >= SEV_GPU_MAX_VMS)
		return;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm, &comm_keystore.valid);
	spin_unlock(&comm_keystore.lock);
	if (have_key)
		return;
	if (atomic_read(&hs_client_attempts[vm]) >= SEV_GPU_HS_MAX_ATTEMPTS)
		return;
	if (atomic_cmpxchg(&hs_client_busy[vm], 0, 1) != 0)
		return;

	atomic_inc(&hs_client_attempts[vm]);
	ret = sev_gpu_hs_client_run(vm);
	atomic_set(&hs_client_busy[vm], 0);
	if (ret)
		pr_warn_ratelimited("sev_gpu: auto-mTLS: client handshake attempt failed (%d) for vm %u\n",
				    ret, vm);
}

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
#define SEV_KMB_NV_OK   0u
#define SEV_KMB_NV_ERR  RPC_FWD_ERR	/* generic NV error reported to CUDA */

static atomic_t sev_gpu_kmb_pull_seq = ATOMIC_INIT(0);

/*
 * Manager: seal a freshly-fetched channel CC_KMB under the requesting client's
 * comm key. Fills the caller-provided nonce/tag/ciphertext buffers and reports
 * the AAD-binding seq + keyspace. The plaintext KMB (kmb_plain) is the manager's
 * private, SEV-encrypted memory; only the sealed ciphertext ever crosses the
 * host-visible ivshmem region.
 */
static u32 sev_gpu_kmb_seal_impl(u32 client_id, u32 channel_id,
				 const void *kmb_plain, u32 kmb_len,
				 void *out_nonce, void *out_tag, void *out_ct,
				 u32 *out_seq, u32 *out_keyspace)
{
	struct sev_gpu_kmb_aad aad;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce[SEV_GPU_KMB_NONCE_LEN];
	u8 tag[SEV_GPU_KMB_TAG_LEN];
	u8 ct[sizeof(struct sev_cc_kmb)];
	u32 seq, keyspace = 0;
	bool have_key;
	int ret;

	if (!kmb_plain || !out_nonce || !out_tag || !out_ct || !out_seq ||
	    !out_keyspace)
		return SEV_KMB_NV_ERR;
	if (kmb_len != sizeof(struct sev_cc_kmb) || client_id >= SEV_GPU_MAX_VMS)
		return SEV_KMB_NV_ERR;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(client_id, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[client_id], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key && auto_mtls &&
	    sev_gpu_wait_comm_key(client_id, auto_mtls_wait_ms)) {
		spin_lock(&comm_keystore.lock);
		have_key = test_bit(client_id, &comm_keystore.valid);
		if (have_key)
			memcpy(key, comm_keystore.key[client_id],
			       SEV_GPU_COMM_KEY_LEN);
		spin_unlock(&comm_keystore.lock);
	}
	if (!have_key) {
		pr_warn("sev_gpu: GET_KMB seal: no comm key for vm %u\n", client_id);
		return SEV_KMB_NV_ERR;
	}

	seq = (u32)atomic_inc_return(&sev_gpu_kmb_pull_seq);
	get_random_bytes(nonce, sizeof(nonce));
	memcpy(ct, kmb_plain, kmb_len);

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = client_id;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Seal in place: ct now holds ciphertext, tag holds the GCM tag. */
	ret = sev_gpu_aead(true, key, nonce, &aad, sizeof(aad), ct, kmb_len, tag);
	memzero_explicit(key, sizeof(key));
	if (ret) {
		memzero_explicit(ct, sizeof(ct));
		pr_warn("sev_gpu: GET_KMB seal: aead failed %d\n", ret);
		return SEV_KMB_NV_ERR;
	}

	memcpy(out_nonce, nonce, sizeof(nonce));
	memcpy(out_tag, tag, sizeof(tag));
	memcpy(out_ct, ct, kmb_len);
	*out_seq = seq;
	*out_keyspace = keyspace;
	memzero_explicit(ct, sizeof(ct));

	pr_info("sev_gpu: GET_KMB sealed ch 0x%x for vm %u seq %u\n",
		channel_id, client_id, seq);
	return SEV_KMB_NV_OK;
}

/*
 * Client: unseal a sealed CC_KMB delivered in a GET_KMB reply and install it in
 * the per-channel keystore (consumed later by the data-plane crypto). On success
 * the plaintext CC_KMB is written to kmb_out (a kernel buffer the caller copies
 * to CUDA's params). Authentication failure (tampered/mis-keyed ciphertext)
 * returns an error and installs nothing.
 */
static u32 sev_gpu_kmb_install_impl(u32 channel_id, u32 seq, u32 keyspace,
				    const void *nonce, const void *tag,
				    const void *ct, u32 ct_len, void *kmb_out)
{
	struct sev_gpu_dev *d = ctrl_dev;
	struct sev_gpu_kmb_aad aad;
	struct sev_cc_kmb kmb;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce_b[SEV_GPU_KMB_NONCE_LEN];
	u8 tag_b[SEV_GPU_KMB_TAG_LEN];
	u32 vm;
	bool have_key;
	int ret, slot = -1, j;

	if (!d || d->is_manager)
		return SEV_KMB_NV_ERR;
	if (!nonce || !tag || !ct || !kmb_out || ct_len != sizeof(kmb))
		return SEV_KMB_NV_ERR;

	vm = d->comm_vm_id;

	/*
	 * If the in-kernel handshake has not yet delivered the comm key, drive
	 * it now (keyed by our slot index) and re-read the negotiated vm.
	 */
	if (auto_mtls) {
		bool ready = false;

		spin_lock(&comm_keystore.lock);
		if (vm < SEV_GPU_MAX_VMS)
			ready = test_bit(vm, &comm_keystore.valid);
		spin_unlock(&comm_keystore.lock);
		if (!ready) {
			sev_gpu_hs_client_maybe_run(d->client_vm_id);
			vm = d->comm_vm_id;
		}
	}
	if (vm >= SEV_GPU_MAX_VMS)
		return SEV_KMB_NV_ERR;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[vm], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key && auto_mtls &&
	    sev_gpu_wait_comm_key(vm, auto_mtls_wait_ms)) {
		spin_lock(&comm_keystore.lock);
		have_key = test_bit(vm, &comm_keystore.valid);
		if (have_key)
			memcpy(key, comm_keystore.key[vm], SEV_GPU_COMM_KEY_LEN);
		spin_unlock(&comm_keystore.lock);
	}
	if (!have_key) {
		pr_warn("sev_gpu: GET_KMB install: no comm key for vm %u\n", vm);
		return SEV_KMB_NV_ERR;
	}

	memcpy(&kmb, ct, ct_len);
	memcpy(nonce_b, nonce, sizeof(nonce_b));
	memcpy(tag_b, tag, sizeof(tag_b));

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = vm;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Unseal in place; auth failure => nonzero ret, kmb is meaningless. */
	ret = sev_gpu_aead(false, key, nonce_b, &aad, sizeof(aad),
			   &kmb, ct_len, tag_b);
	memzero_explicit(key, sizeof(key));
	if (ret) {
		memzero_explicit(&kmb, sizeof(kmb));
		pr_warn("sev_gpu: GET_KMB install: unseal failed %d (auth?)\n", ret);
		return SEV_KMB_NV_ERR;
	}

	/* Install into the client's per-channel keystore (reuse or first free). */
	spin_lock(&client_kmb_store.lock);
	for (j = 0; j < SEV_GPU_MAX_CHANNELS_PER_VM; j++) {
		struct sev_client_chan *e = &client_kmb_store.c[j];

		if (e->valid && e->channel_id == channel_id) {
			slot = j;
			break;
		}
		if (slot < 0 && !e->valid)
			slot = j;
	}
	if (slot >= 0) {
		struct sev_client_chan *e = &client_kmb_store.c[slot];

		e->kmb        = kmb;
		e->channel_id = channel_id;
		e->keyspace   = keyspace;
		e->generation = seq;	/* KMB epoch */
		e->ctr_h2d    = 0;	/* fresh key => reset IV counters */
		e->ctr_d2h    = 0;
		e->valid      = true;
	}
	spin_unlock(&client_kmb_store.lock);
	if (slot < 0) {
		memzero_explicit(&kmb, sizeof(kmb));
		pr_warn("sev_gpu: GET_KMB install: keystore full (ch 0x%x)\n",
			channel_id);
		return SEV_KMB_NV_ERR;
	}

	memcpy(kmb_out, &kmb, ct_len);
	memzero_explicit(&kmb, sizeof(kmb));

	pr_info("sev_gpu: GET_KMB installed ch 0x%x vm %u seq %u\n",
		channel_id, vm, seq);
	return SEV_KMB_NV_OK;
}

/*
 * Automatic KMB-handshake worker. Driven from SET_COMM_KEY: the manager assigns
 * a pre-provisioned channel of hs_keyspace to each pending client and seals its
 * KMB; the client receives + installs the KMB its manager posted. All the
 * blocking waits happen here, off the ioctl path.
 */
static void sev_gpu_hs_work(struct work_struct *w)
{
	struct sev_gpu_dev *d = ctrl_dev;
	unsigned int to_ms = hs_timeout_ms;

	if (!d)
		return;

	if (d->is_manager) {
		unsigned long pend;
		unsigned int vm;

		spin_lock(&hs_state.lock);
		pend = hs_state.pending;
		hs_state.pending = 0;
		spin_unlock(&hs_state.lock);

		for_each_set_bit(vm, &pend, SEV_GPU_MAX_VMS) {
			u32 channel_id = 0;
			u8 fp[8];
			int rc;

			rc = sev_gpu_assign_channel((u8)vm, hs_keyspace, 0, 0, 0,
						    &channel_id, NULL, NULL);
			if (rc) {
				pr_warn("sev_gpu: auto-handshake VM%u assign failed (%d) -- provision keyspace %u first?\n",
					vm, rc, hs_keyspace);
				continue;
			}
			rc = sev_gpu_send_kmb((u8)vm, channel_id, to_ms, fp);
			if (rc)
				pr_warn("sev_gpu: auto-handshake VM%u send KMB ch %u failed (%d)\n",
					vm, channel_id, rc);
			else
				pr_info("sev_gpu: auto-handshake VM%u channel %u KMB delivered (fp %02x%02x%02x%02x)\n",
					vm, channel_id, fp[0], fp[1], fp[2],
					fp[3]);
		}
	} else {
		u32 channel_id = 0, keyspace = 0;
		u8 fp[8];
		int rc = sev_gpu_recv_kmb(d, to_ms, &channel_id, &keyspace, fp);

		if (rc)
			pr_warn("sev_gpu: auto-handshake client recv KMB failed (%d)\n",
				rc);
		else
			pr_info("sev_gpu: auto-handshake client installed channel %u (keyspace %u)\n",
				channel_id, keyspace);
	}
}

static long sev_gpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
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

static const struct file_operations sev_gpu_fops = {
	.owner          = THIS_MODULE,
	.open           = sev_gpu_open,
	.release        = sev_gpu_release,
	.mmap           = sev_gpu_mmap,
	.unlocked_ioctl = sev_gpu_ioctl,
	.compat_ioctl   = sev_gpu_ioctl,
};

static int sev_gpu_setup_chardev(struct sev_gpu_dev *d)
{
	int ret;

	/* Control device uses minor 0 of the shared region + the shared class. */
	d->devt = MKDEV(MAJOR(sev_gpu_devt_base), MINOR(sev_gpu_devt_base));
	cdev_init(&d->cdev, &sev_gpu_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret)
		return ret;

	d->device = device_create(sev_gpu_class, NULL, d->devt, NULL, DEVICE_NAME);
	if (IS_ERR(d->device)) {
		ret = PTR_ERR(d->device);
		cdev_del(&d->cdev);
		return ret;
	}
	return 0;
}

static void sev_gpu_teardown_chardev(struct sev_gpu_dev *d)
{
	device_destroy(sev_gpu_class, d->devt);
	cdev_del(&d->cdev);
}

/* ------------------------------------------------------------------ */
/* Private per-VM data device (/dev/sev_gpu_dataN)                     */
/* ------------------------------------------------------------------ */

/* Manager: initialise a fresh data-region header (identity binding). */
static void data_init_header(struct sev_gpu_data_dev *dd)
{
	sev_gpu_data_header_t h;

	memset(&h, 0, sizeof(h));
	h.magic        = SEV_GPU_DATA_MAGIC;
	h.version      = SEV_GPU_DATA_VERSION;
	h.pool_index   = dd->pool_index;
	h.owner_vm_id  = dd->pool_index;	/* default identity binding */
	h.state        = SEV_GPU_DATA_BOUND;
	h.region_size  = dd->mem_size;
	h.payload_off  = SEV_GPU_DATA_HEADER_SIZE;
	h.payload_size = (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE) ?
				dd->mem_size - SEV_GPU_DATA_HEADER_SIZE : 0;
	memcpy_toio(dd->mem, &h, sizeof(h));
}

static int sev_gpu_data_open(struct inode *inode, struct file *filp)
{
	filp->private_data = container_of(inode->i_cdev,
					  struct sev_gpu_data_dev, cdev);
	return 0;
}

static int sev_gpu_data_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* Map the WHOLE private region (header + payload) -- already isolated by QEMU. */
static int sev_gpu_data_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sev_gpu_data_dev *dd = filp->private_data;
	unsigned long vsize = vma->vm_end - vma->vm_start;

	if (!dd || !dd->mem_phys)
		return -ENODEV;
	if (vsize > dd->mem_size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_decrypted(pgprot_noncached(vma->vm_page_prot));
	return io_remap_pfn_range(vma, vma->vm_start,
				  dd->mem_phys >> PAGE_SHIFT,
				  vsize, vma->vm_page_prot);
}

static long sev_gpu_data_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct sev_gpu_data_dev *dd = filp->private_data;
	void __user *argp = (void __user *)arg;
	sev_gpu_data_header_t h;

	if (!dd)
		return -ENODEV;

	switch (cmd) {
	case SEV_GPU_IOC_DATA_INFO: {
		sev_gpu_ioctl_data_info_t info;

		memcpy_fromio(&h, dd->mem, sizeof(h));
		memset(&info, 0, sizeof(info));
		info.pool_index   = dd->pool_index;
		info.is_manager   = dd->is_manager ? 1 : 0;
		info.region_size  = dd->mem_size;
		info.payload_off  = SEV_GPU_DATA_HEADER_SIZE;
		info.payload_size = (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE) ?
					dd->mem_size - SEV_GPU_DATA_HEADER_SIZE : 0;
		if (h.magic == SEV_GPU_DATA_MAGIC) {
			info.owner_vm_id = h.owner_vm_id;
			info.state       = h.state;
		} else {
			info.owner_vm_id = SEV_GPU_DATA_OWNER_NONE;
			info.state       = SEV_GPU_DATA_FREE;
		}
		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_DATA_BIND: {
		sev_gpu_ioctl_data_bind_t bind;

		if (!dd->is_manager)
			return -EPERM;	/* only the manager (re)binds regions */
		if (copy_from_user(&bind, argp, sizeof(bind)))
			return -EFAULT;

		/* Scrub the payload before changing ownership so a reused
		 * region can't leak the previous owner's bytes. */
		if (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE)
			memset_io(dd->mem + SEV_GPU_DATA_HEADER_SIZE, 0,
				  dd->mem_size - SEV_GPU_DATA_HEADER_SIZE);

		memcpy_fromio(&h, dd->mem, sizeof(h));
		if (h.magic != SEV_GPU_DATA_MAGIC) {
			memset(&h, 0, sizeof(h));
			h.magic        = SEV_GPU_DATA_MAGIC;
			h.version      = SEV_GPU_DATA_VERSION;
			h.pool_index   = dd->pool_index;
			h.region_size  = dd->mem_size;
			h.payload_off  = SEV_GPU_DATA_HEADER_SIZE;
			h.payload_size = (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE) ?
						dd->mem_size - SEV_GPU_DATA_HEADER_SIZE : 0;
		}
		h.owner_vm_id = bind.owner_vm_id;
		h.state = (bind.owner_vm_id == SEV_GPU_DATA_OWNER_NONE) ?
				SEV_GPU_DATA_FREE : SEV_GPU_DATA_BOUND;
		h.data_len = 0;
		h.seq = 0;
		memcpy_toio(dd->mem, &h, sizeof(h));

		pr_info("sev_gpu: data[%u] bound to owner %u\n",
			dd->pool_index, bind.owner_vm_id);
		break;
	}

	default:
		return -ENOTTY;
	}
	return 0;
}

static const struct file_operations sev_gpu_data_fops = {
	.owner          = THIS_MODULE,
	.open           = sev_gpu_data_open,
	.release        = sev_gpu_data_release,
	.mmap           = sev_gpu_data_mmap,
	.unlocked_ioctl = sev_gpu_data_ioctl,
	.compat_ioctl   = sev_gpu_data_ioctl,
};

static int sev_gpu_data_setup_chardev(struct sev_gpu_data_dev *dd)
{
	int ret;

	dd->devt = MKDEV(MAJOR(sev_gpu_devt_base),
			 MINOR(sev_gpu_devt_base) + 1 + dd->pool_index);
	cdev_init(&dd->cdev, &sev_gpu_data_fops);
	dd->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dd->cdev, dd->devt, 1);
	if (ret)
		return ret;

	dd->device = device_create(sev_gpu_class, NULL, dd->devt, NULL,
				   "sev_gpu_data%u", dd->pool_index);
	if (IS_ERR(dd->device)) {
		ret = PTR_ERR(dd->device);
		cdev_del(&dd->cdev);
		return ret;
	}
	return 0;
}

static void sev_gpu_data_teardown_chardev(struct sev_gpu_data_dev *dd)
{
	device_destroy(sev_gpu_class, dd->devt);
	cdev_del(&dd->cdev);
}

/* ------------------------------------------------------------------ */
/* RM-RPC forwarding (client) + replay (manager)                       */
/* ------------------------------------------------------------------ */

/*
 * Lazy synthetic-GPU attach (defined later): retried from the client forward
 * path so /dev/nvidia0 materializes once the manager has published its GPU
 * identity, even if that happens after this client bound its control device.
 */
static void sev_gpu_client_attach_gpu(struct sev_gpu_dev *d);

/*
 * Client side: forward one RM escape ioctl to the manager and block for the
 * reply. Marshals {cmd, arg[0..size)} into this client's mailbox, kicks the
 * manager's RPC doorbell, then polls the mailbox until the manager publishes a
 * reply. Returns the NV_STATUS the manager produced (0 == NV_OK), or
 * RPC_FWD_ERR on a transport failure.
 *
 * Nested params: escape structs embed user pointers (e.g. NVOS54.params,
 * NVOS21.pAllocParms). For each such pointer this deep-copies the pointee into
 * the slot's in-slot staging area, records a buffers[] descriptor, and rewrites
 * the in-slot copy of the pointer to its staging offset. On reply it copies any
 * OUT pointee back to user space and restores the caller's original pointer in
 * @arg so the CUDA process sees its own pointer value unchanged. Runs in the
 * calling process's context, so copy_{from,to}_user target the CUDA process.
 */
static u32 sev_gpu_rm_forward(u32 cmd, void *arg, u32 size)
{
	/* Local record of what we staged, used to un-marshal the reply safely
	 * without trusting the manager-returned buffers[] for user addresses. */
	struct {
		u64 uptr;		/* original userspace pointer             */
		u64 staging_off;	/* data-region offset of the bytes        */
		u32 size;		/* pointee length                         */
		u32 dir;		/* SEV_GPU_RPC_BUF_*                      */
		u32 struct_off;		/* offset of the pointer field in @arg    */
	} nb[SEV_GPU_RPC_MAX_BUFFERS];
	/* Level-2 embedded output pointer tracking (pointers within pParams). */
	struct {
		u64 uptr;		/* original userspace pointer             */
		u64 staging_off;	/* staging offset of the output buffer    */
		u64 pparams_soff;	/* staging offset of the parent pParams   */
		u32 size;		/* output buffer length                   */
		u32 pptr_off;		/* offset of pointer field within pParams */
		u32 dir;		/* SEV_GPU_RPC_BUF_*                      */
	} nb2[SEV_GPU_RPC_MAX_BUFFERS];
	struct rpc_nested desc[SEV_GPU_RPC_MAX_BUFFERS];
	struct sev_gpu_data_dev *dd;
	sev_gpu_rpc_slot_t *slot;
	void __iomem *mb;
	unsigned long deadline;
	u64 stage_base, stage_used = 0;
	u32 state = SEV_GPU_RPC_IDLE;
	u32 status = RPC_FWD_ERR;
	int ndesc, i, nbn = 0, nb2n = 0;
	bool kparams = false;
	u8 vm;

	if (size > SEV_GPU_RPC_INLINE_MAX)
		return RPC_FWD_ERR;
	if (!ctrl_dev)
		return RPC_FWD_ERR;

	/*
	 * First client<->manager contact: establish the comm key in-kernel via
	 * the ECDHE-PSK handshake before forwarding, so the later sealed-KMB
	 * (GET_KMB) path finds a key. Additive and best-effort -- on failure we
	 * still forward (the seal/install paths remain fail-closed).
	 */
	if (auto_mtls && !ctrl_dev->is_manager)
		sev_gpu_hs_client_maybe_run(ctrl_dev->client_vm_id);

	/*
	 * Materialize the synthetic /dev/nvidia0 lazily. The manager publishes
	 * its GPU identity on its own boot timeline, which may be AFTER this
	 * client bound its control device -- in which case the probe-time attach
	 * found nothing and CUDA sees no device. Reaching this forward path means
	 * the client has real RM traffic (and, with auto-mTLS, the manager is
	 * up), so retry the attach here -- before CUDA enumerates GPUs. The call
	 * is idempotent and a cheap early-out once the device is registered.
	 */
	if (!ctrl_dev->is_manager)
		sev_gpu_client_attach_gpu(ctrl_dev);

	if (cmd == RPC_NV_ESC_RM_CONTROL && size >= 12) {
		u32 ctrl_cmd = 0;
		memcpy(&ctrl_cmd, (const u8 *)arg + 8, 4);
		/*
		 * SEV-INSTR: stamp the forwarding origin. For the repeated
		 * MC_SERVICE_INTERRUPTS (0x20801702) poll, comm/pid disambiguates a
		 * libcuda userspace ioctl (comm is the CUDA process) from an in-kernel
		 * UVM wait re-issuing the control (comm is a kernel worker / the
		 * process blocked in the UVM ioctl).
		 */
		pr_info("sev_gpu: RM-RPC fwd CTRL ctrl_cmd=0x%x size=%u vm=%u origin_pid=%d origin_comm=%s\n",
			ctrl_cmd, size,
			ctrl_dev ? ctrl_dev->client_vm_id : 0xFF,
			current->pid, current->comm);
	} else if (cmd == RPC_NV_ESC_RM_ALLOC && size >= 16) {
		u32 hClass = 0;
		memcpy(&hClass, (const u8 *)arg + 12, 4);
		/*
		 * For NVOS64 allocs also surface CUDA's RAW pAllocParms pointer
		 * (offset 16) and paramsSize (offset 32) as the CLIENT sees them,
		 * BEFORE any staging.  This distinguishes "CC mode passed a NULL
		 * pAllocParms" from "params exist but were not staged/read" for
		 * classes like the TSG (0xa06c) where the manager observes 0/0.
		 */
		if (size >= RPC_NVOS64_SIZE) {
			u64 palloc = 0;
			u32 psz = 0;
			memcpy(&palloc, (const u8 *)arg + RPC_NVOS21_PALLOC_OFF, 8);
			memcpy(&psz, (const u8 *)arg + RPC_NVOS64_PARAMSSIZE_OFF, 4);
			pr_info("sev_gpu: RM-RPC fwd ALLOC hClass=0x%x size=%u vm=%u pAllocParms=0x%llx paramsSize=%u\n",
				hClass, size,
				ctrl_dev ? ctrl_dev->client_vm_id : 0xFF,
				(unsigned long long)palloc, psz);
		} else {
			pr_info("sev_gpu: RM-RPC fwd ALLOC hClass=0x%x size=%u vm=%u\n",
				hClass, size,
				ctrl_dev ? ctrl_dev->client_vm_id : 0xFF);
		}
	} else {
		pr_info("sev_gpu: RM-RPC fwd cmd=0x%x size=%u vm=%u\n",
			cmd, size,
			ctrl_dev ? ctrl_dev->client_vm_id : 0xFF);
	}

	ndesc = rpc_nested_layout(cmd, arg, size, desc);
	if (ndesc < 0) {
		pr_warn_ratelimited("sev_gpu: RM-RPC unsupported nested cmd=0x%x size=%u\n",
				    cmd, size);
		return RPC_FWD_ERR;
	}

	/*
	 * FINN fast path: for a control the client already flattened into a
	 * self-describing blob (NVOS54_FLAGS_FINN_SERIALIZED set), the params
	 * pointer references KERNEL memory the client owns, not user space. The
	 * single nested buffer is then staged/un-staged with memcpy instead of
	 * copy_{from,to}_user. The blob is opaque here -- the manager's RM
	 * deserializes it natively on replay.
	 */
	if (cmd == RPC_NV_ESC_RM_CONTROL && arg && size >= RPC_NVOS54_SIZE) {
		u32 flags = 0;

		memcpy(&flags, (const u8 *)arg + RPC_NVOS54_FLAGS_OFF, 4);
		kparams = (flags & RPC_NVOS54_FLAGS_FINN_SERIALIZED) != 0;
	}

	/* Use this client's own slot in the shared control-BAR mailbox. */
	vm = ctrl_dev->client_vm_id;
	mb = rpc_ctrl_mailbox(vm);
	if (!mb) {
		pr_warn_ratelimited("sev_gpu: RM-RPC no mailbox for vm=%u\n", vm);
		return RPC_FWD_ERR;
	}

	/*
	 * Zero-copy nested staging lives in this client's own data region (the
	 * single region this VM attaches). The manager maps the same region and
	 * points the RM at the staged bytes in place, so no copy happens on the
	 * manager side -- only the unavoidable copy_{from,to}_user below.
	 */
	dd = (num_data_devs > 0) ? data_devs[0] : NULL;
	stage_base = (dd && dd->mem) ? rpc_staging_base(dd->mem_size) : 0;
	if (ndesc > 0 && !stage_base) {
		pr_warn_ratelimited("sev_gpu: RM-RPC no data region for nested cmd=0x%x\n",
				    cmd);
		return RPC_FWD_ERR;
	}

	/*
	 * Publish the client's BAR2 GPA once so the manager's shadow_db can
	 * return a pLinearAddress in the CLIENT's ivshmem address space.  By
	 * this point the manager has already initialised the data header (it
	 * booted earlier), so writing the reserved field is safe.
	 */
	if (dd && dd->mem && dd->mem_phys) {
		static bool client_phys_published;

		if (!READ_ONCE(client_phys_published)) {
			u64 phys = (u64)dd->mem_phys;

			memcpy_toio((u8 __iomem *)dd->mem +
				    offsetof(sev_gpu_data_header_t, client_mem_phys),
				    &phys, sizeof(phys));
			wmb();
			WRITE_ONCE(client_phys_published, true);
			pr_info("sev_gpu: published client_mem_phys=0x%llx\n",
				(unsigned long long)phys);
		}
	}

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return RPC_FWD_ERR;

	mutex_lock(&rpc_client_lock);

	slot->magic             = SEV_GPU_RPC_MAGIC;
	slot->version           = SEV_GPU_RPC_VERSION;
	slot->client_vm_id      = vm;
	slot->state             = SEV_GPU_RPC_IDLE;
	slot->seq               = ++rpc_client_seq;
	slot->cmd               = cmd;
	slot->arg_size          = size;
	slot->n_buffers         = 0;
	/* Carry our data-region GPA so the manager can populate its cache even
	 * when the data-header write path is unavailable (no data device, or
	 * SEV-SNP C-bit preventing the header write from being visible). */
	slot->client_data_phys  = (dd && dd->mem_phys) ? (u64)dd->mem_phys : 0;
	if (size && arg)
		memcpy(slot->inline_arg, arg, size);

	/* Deep-copy each embedded pointer into the data-region staging window. */
	for (i = 0; i < ndesc; i++) {
		u64 uptr = 0, soff;
		u32 off = desc[i].ptr_off, sz = desc[i].size;

		if (off + 8 > size)
			continue;
		memcpy(&uptr, (const u8 *)arg + off, 8);
		if (!uptr || !sz)
			continue;		/* NULL pointer / empty pointee */

		if (stage_used + ALIGN(sz, 8) > SEV_GPU_RPC_DATA_STAGING_SIZE) {
			pr_warn_ratelimited("sev_gpu: RM-RPC staging overflow cmd=0x%x need=%llu\n",
					    cmd, stage_used + ALIGN(sz, 8));
			status = RPC_FWD_ERR;
			goto out;
		}
		soff = stage_base + stage_used;

		if (desc[i].dir & SEV_GPU_RPC_BUF_IN) {
			if (kparams) {
				/* params is a kernel-resident FINN blob */
				memcpy_toio((u8 __iomem *)dd->mem + soff,
					    (const void *)(uintptr_t)uptr, sz);
			} else {
				void *tmp = kvmalloc(sz, GFP_KERNEL);

				if (!tmp) {
					status = RPC_FWD_ERR;
					goto out;
				}
				if (copy_from_user(tmp, (const void __user *)(uintptr_t)uptr, sz)) {
					kvfree(tmp);
					status = RPC_FWD_ERR;
					goto out;
				}
				memcpy_toio((u8 __iomem *)dd->mem + soff, tmp, sz);
				kvfree(tmp);
			}
		}

		slot->buffers[slot->n_buffers].client_ptr  = uptr;
		slot->buffers[slot->n_buffers].staging_off = soff;
		slot->buffers[slot->n_buffers].size        = sz;
		slot->buffers[slot->n_buffers].dir         = desc[i].dir;
		slot->buffers[slot->n_buffers].struct_off  = off;
		slot->buffers[slot->n_buffers].parent      = SEV_GPU_RPC_PARENT_TOPLEVEL;
		slot->n_buffers++;

		/* In the posted copy, the pointer becomes its data-region offset. */
		memcpy(slot->inline_arg + off, &soff, 8);

		nb[nbn].uptr        = uptr;
		nb[nbn].staging_off = soff;
		nb[nbn].size        = sz;
		nb[nbn].dir         = desc[i].dir;
		nb[nbn].struct_off  = off;
		nbn++;
		stage_used += ALIGN(sz, 8);
	}

	/*
	 * Level-2 embedded pointer staging: for controls whose pParams struct
	 * contains further pointers into the client's userspace, allocate staging
	 * space for each pointee, patch the staged pParams to use staging offsets
	 * in place of userspace addresses, and add level-2 buffer descriptors. The
	 * manager resolves those offsets to kernel VAs before calling the RM, so
	 * the RM reads/writes the data region (kernel memory) rather than the
	 * client's unmapped userspace.
	 *
	 * The embedded-pointer layout is table-driven (rpc_ctrl_policy); adding a
	 * new control is one rpc_ctrl_policies[] entry. NV0000_CTRL_CMD_SYSTEM_
	 * GET_BUILD_VERSION is the first (three OUT string buffers).
	 */
	if (cmd == RPC_NV_ESC_RM_CONTROL && !kparams && nbn > 0) {
		u32 ctrl_cmd = 0;
		const struct rpc_ctrl_policy *pol;

		memcpy(&ctrl_cmd, (const u8 *)arg + RPC_NVOS54_CMD_OFF, 4);
		pol = rpc_ctrl_policy(ctrl_cmd);
		if (pol && pol->disp == RPC_CTRL_LEVEL2) {
			u64 pparams_soff = nb[0].staging_off;
			u32 pparams_sz   = nb[0].size;
			u32 f;

			for (f = 0; f < pol->n_fields &&
				     nb2n < SEV_GPU_RPC_MAX_BUFFERS &&
				     slot->n_buffers < SEV_GPU_RPC_MAX_BUFFERS; f++) {
				const struct rpc_embedded_field *ef = &pol->fields[f];
				u64 orig_ptr = 0, svoff;
				u32 fsz;

				/* Size: a u32 field within pParams, or a fixed value. A
				 * field with elem_size!=0 is an element count, converted
				 * to bytes in u64 to avoid u32 multiply overflow. */
				if (ef->size_off == RPC_SIZE_FIXED) {
					fsz = ef->fixed_size;
				} else {
					u32 count = 0;
					u64 bytes;

					if (ef->size_off + 4 > pparams_sz)
						continue;
					memcpy_fromio(&count, (u8 __iomem *)dd->mem +
						      pparams_soff + ef->size_off, 4);
					bytes = ef->elem_size ?
						(u64)count * ef->elem_size : (u64)count;
					if (bytes > SEV_GPU_RPC_DATA_STAGING_SIZE)
						continue;
					fsz = (u32)bytes;
				}
				if (fsz == 0 || fsz > SEV_GPU_RPC_DATA_STAGING_SIZE)
					continue;
				if (ef->pptr_off + 8 > pparams_sz)
					continue;

				memcpy_fromio(&orig_ptr, (u8 __iomem *)dd->mem +
					      pparams_soff + ef->pptr_off, 8);
				if (!orig_ptr)
					continue;
				if (stage_used + ALIGN(fsz, 8) > SEV_GPU_RPC_DATA_STAGING_SIZE) {
					pr_warn_ratelimited(
					    "sev_gpu: RM-RPC staging overflow for embedded ptrs\n");
					status = RPC_FWD_ERR;
					goto out;
				}
				svoff = stage_base + stage_used;
				stage_used += ALIGN(fsz, 8);

				/* IN pointee: copy the client's bytes into staging now. */
				if (ef->dir & SEV_GPU_RPC_BUF_IN) {
					void *tmp = kvmalloc(fsz, GFP_KERNEL);

					if (!tmp) {
						status = RPC_FWD_ERR;
						goto out;
					}
					if (copy_from_user(tmp,
					    (const void __user *)(uintptr_t)orig_ptr, fsz)) {
						kvfree(tmp);
						status = RPC_FWD_ERR;
						goto out;
					}
					memcpy_toio((u8 __iomem *)dd->mem + svoff, tmp, fsz);
					kvfree(tmp);
				}

				/* Patch staged pParams: replace userspace ptr with staging offset.
				 * Manager rewrites this to a kernel VA before replay. */
				memcpy_toio((u8 __iomem *)dd->mem + pparams_soff + ef->pptr_off,
					    &svoff, 8);

				slot->buffers[slot->n_buffers].client_ptr  = orig_ptr;
				slot->buffers[slot->n_buffers].staging_off = svoff;
				slot->buffers[slot->n_buffers].size        = fsz;
				slot->buffers[slot->n_buffers].dir         = ef->dir;
				slot->buffers[slot->n_buffers].struct_off  = ef->pptr_off;
				slot->buffers[slot->n_buffers].parent      = 0; /* pParams buf idx */
				slot->n_buffers++;

				nb2[nb2n].uptr         = orig_ptr;
				nb2[nb2n].staging_off  = svoff;
				nb2[nb2n].pparams_soff = pparams_soff;
				nb2[nb2n].size         = fsz;
				nb2[nb2n].pptr_off     = ef->pptr_off;
				nb2[nb2n].dir          = ef->dir;
				nb2n++;
			}
		}
	}

	/* Publish the payload first, then flip state to REQUEST last. */
	memcpy_toio(mb, slot, sizeof(*slot));
	wmb();
	iowrite32(SEV_GPU_RPC_REQUEST, mb + RPC_STATE_OFF);

	/* Kick the manager, then poll for the reply (NAPI-style: kick + poll).
	 * The ring is best-effort -- the manager's replay thread also polls, so
	 * progress does not depend on the manager being peer 0. */
	ivshmem_ring(ctrl_dev, sev_gpu_manager_peer(ctrl_dev), IVSHMEM_VECTOR_RPC);

	deadline = jiffies + msecs_to_jiffies(RPC_TIMEOUT_MS);
	for (;;) {
		state = ioread32(mb + RPC_STATE_OFF);
		if (state == SEV_GPU_RPC_REPLY)
			break;
		if (time_after(jiffies, deadline))
			break;
		if (signal_pending(current))
			break;
		usleep_range(20, 50);
	}

	if (state == SEV_GPU_RPC_REPLY) {
		memcpy_fromio(slot, mb, sizeof(*slot));
		status = (u32)slot->rm_status;
		if (size && arg)
			memcpy(arg, slot->inline_arg, size);

		/* Log every RM_CONTROL reply with its ctrl_cmd and op_status so we
		 * can see the full CUDA init sequence without rate-limiter blindness.
		 * NVOS54.status is at offset 28; NVOS64.status is at offset 40. */
		if (cmd == RPC_NV_ESC_RM_CONTROL && size >= 32) {
			u32 ctrl_cmd = 0, op_status = 0;
			memcpy(&ctrl_cmd,  slot->inline_arg + 8,  4);
			memcpy(&op_status, slot->inline_arg + 28, 4);
			if (op_status || status)
				pr_warn("sev_gpu: RM-RPC CTRL reply ctrl_cmd=0x%x"
					" op=0x%x transport=0x%x\n",
					ctrl_cmd, op_status, status);
			else
				pr_info("sev_gpu: RM-RPC CTRL reply ctrl_cmd=0x%x ok\n",
					ctrl_cmd);
		} else if (cmd == RPC_NV_ESC_RM_ALLOC && size >= 44) {
			u32 hClass = 0, op_status = 0;
			memcpy(&hClass,    slot->inline_arg + 12, 4);
			memcpy(&op_status, slot->inline_arg + 40, 4);
			if (op_status || status)
				pr_warn("sev_gpu: RM-RPC ALLOC reply hClass=0x%x"
					" op=0x%x transport=0x%x\n",
					hClass, op_status, status);
			else
				pr_info("sev_gpu: RM-RPC ALLOC reply hClass=0x%x ok\n",
					hClass);
		} else if (cmd == 0x4e /* NV_ESC_RM_MAP_MEMORY */ && size >= 44) {
			/*
			 * The manager intercepted MAP_MEMORY for the
			 * HOPPER_USERMODE_A object and returned the shadow
			 * doorbell GPA as pLinearAddress.  libcuda.so will call
			 * mmap(fd, 0) immediately after (not mmap(fd, shadow_gpa)
			 * -- offset=0 is the standard NVIDIA convention).  Store
			 * the PFN so sev_gpu_mmap_redirect_impl can serve it.
			 */
			u32 rm_status = 0;
			u64 shadow_gpa = 0;
			memcpy(&shadow_gpa, slot->inline_arg + 32, 8); /* pLinearAddress */
			memcpy(&rm_status,  slot->inline_arg + 40, 4); /* status */
			if (!rm_status && shadow_gpa) {
				WRITE_ONCE(doorbell_mmap_pfn,
					   (unsigned long)(shadow_gpa >> PAGE_SHIFT));
				pr_info("sev_gpu: MAP_MEMORY reply: stored doorbell"
					" pfn=0x%lx (shadow_gpa=0x%llx)\n",
					(unsigned long)(shadow_gpa >> PAGE_SHIFT),
					(unsigned long long)shadow_gpa);
			} else {
				pr_warn("sev_gpu: MAP_MEMORY reply: shadow_gpa=0x%llx"
					" rm_status=0x%x transport=0x%x\n",
					(unsigned long long)shadow_gpa,
					rm_status, status);
			}
		} else {
			if (status)
				pr_warn("sev_gpu: RM-RPC reply cmd=0x%x transport=0x%x\n",
					cmd, status);
			else
				pr_info("sev_gpu: RM-RPC reply cmd=0x%x ok\n", cmd);
		}

		/* Level-2 embedded pointers first: deliver each OUT pointee to the
		 * client's userspace buffer, then restore the caller's original
		 * userspace pointer inside the staged pParams so the pParams we copy
		 * back to CUDA (via the INOUT level-1 buffer below) carries the
		 * caller's own pointers -- never staging offsets or kernel VAs. */
		for (i = 0; i < nb2n; i++) {
			if (nb2[i].dir & SEV_GPU_RPC_BUF_OUT) {
				void *tmp = kvmalloc(nb2[i].size, GFP_KERNEL);

				if (!tmp) {
					status = RPC_FWD_ERR;
				} else {
					memcpy_fromio(tmp,
						      (u8 __iomem *)dd->mem + nb2[i].staging_off,
						      nb2[i].size);
					if (copy_to_user((void __user *)(uintptr_t)nb2[i].uptr,
							 tmp, nb2[i].size))
						status = RPC_FWD_ERR;
					kvfree(tmp);
				}
			}
			/* Restore the caller's original pointer in the staged pParams. */
			memcpy_toio((u8 __iomem *)dd->mem +
				    nb2[i].pparams_soff + nb2[i].pptr_off,
				    &nb2[i].uptr, 8);
		}

		/* Copy OUT pointees back to user space (the RM wrote them in place
		 * in our data region) and restore the caller's original pointer
		 * values -- never leak staging offsets to CUDA. */
		for (i = 0; i < nbn; i++) {
			if (nb[i].dir & SEV_GPU_RPC_BUF_OUT) {
				if (kparams) {
					/* OUT blob -> client's kernel FINN buffer */
					memcpy_fromio((void *)(uintptr_t)nb[i].uptr,
						      (u8 __iomem *)dd->mem + nb[i].staging_off,
						      nb[i].size);
				} else {
					void *tmp = kvmalloc(nb[i].size, GFP_KERNEL);

					if (!tmp) {
						status = RPC_FWD_ERR;
					} else {
						memcpy_fromio(tmp,
							      (u8 __iomem *)dd->mem + nb[i].staging_off,
							      nb[i].size);
						if (copy_to_user((void __user *)(uintptr_t)nb[i].uptr,
								 tmp, nb[i].size))
							status = RPC_FWD_ERR;
						kvfree(tmp);
					}
				}
			}
			if (arg && nb[i].struct_off + 8 <= size)
				memcpy((u8 *)arg + nb[i].struct_off, &nb[i].uptr, 8);
		}
	} else {
		pr_warn("sev_gpu: RM-RPC timeout (vm=%u cmd=0x%x)\n", vm, cmd);
	}

out:
	/* Release the mailbox for the next call. */
	iowrite32(SEV_GPU_RPC_IDLE, mb + RPC_STATE_OFF);
	mutex_unlock(&rpc_client_lock);
	kfree(slot);
	return status;
}

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

/*
 * Manager: return the CLIENT's shadow doorbell GPA (within the client's
 * ivshmem compute reserve) for client @client_id.  The manager reads
 * client_mem_phys from the shared data header -- a u64 the client writes on
 * its first RPC -- and adds the doorbell-page offset.  Returns 0 if the
 * client has not yet published its GPA.
 */
static u64 sev_gpu_shadow_db_impl(u32 client_id)
{
	struct sev_gpu_data_dev *dd;
	u64 off, client_phys = 0;

	if ((u32)client_id >= (u32)num_data_devs)
		return 0;
	dd = data_devs[client_id];
	if (!dd || !dd->mem || !dd->mem_size)
		return 0;
	off = compute_doorbell_off(dd->mem_size);
	if (!off)
		return 0;

	/* Primary: RPC-slot cache populated by rpc_service_slot() on every
	 * request.  Works even when the client has no data device or when the
	 * data-header write is not visible due to SEV-SNP C-bit state. */
	if (client_id < SEV_GPU_MAX_VMS)
		client_phys = READ_ONCE(client_mem_phys_cache[client_id]);

	/* Fallback: legacy path – client wrote its GPA into the shared data
	 * header directly (only works when both sides map the same file). */
	if (!client_phys)
		memcpy_fromio(&client_phys,
			      (u8 __iomem *)dd->mem +
			      offsetof(sev_gpu_data_header_t, client_mem_phys),
			      sizeof(client_phys));

	if (!client_phys) {
		pr_warn_ratelimited("sev_gpu: shadow_db: client_mem_phys not yet published for VM%u\n",
				    client_id);
		return 0;
	}
	return client_phys + off;
}

/*
 * Manager: carve a page-aligned slice of client @client_id's ivshmem region to
 * back an esc-0x27 OS-descriptor registration.  Fills the manager-view phys
 * (@mgr_phys -- used by nvidia.ko to build the GPU-side OS_PHYS_ADDR
 * descriptor) and the client-view phys (@cli_phys -- the GPA the client maps
 * for the identical shared page).  Bump-only from the per-client cursor within
 * [osdesc_reserve_base, compute_reserve_base).  Returns 0 on success, -EAGAIN
 * if the client's GPA is not yet published, -ENOSPC if the reserve is full,
 * or -EINVAL on a bad client/region.
 */
static int sev_gpu_osdesc_carve_impl(u32 client_id, u64 size,
				     u64 *mgr_phys, u64 *cli_phys)
{
	struct sev_gpu_data_dev *dd;
	u64 base, end, off, npages, client_phys;

	if ((u32)client_id >= (u32)num_data_devs || client_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;
	dd = data_devs[client_id];
	if (!dd || !dd->mem || !dd->mem_size)
		return -EINVAL;

	base = osdesc_reserve_base(dd->mem_size);
	end  = compute_reserve_base(dd->mem_size);
	if (!base || !end || base >= end)
		return -ENOSPC;

	npages = ALIGN(size ? size : PAGE_SIZE, PAGE_SIZE);

	client_phys = READ_ONCE(client_mem_phys_cache[client_id]);
	if (!client_phys)
		return -EAGAIN;

	mutex_lock(&reg_lock);
	off = osdesc_carve_cursor[client_id];
	if (off < base)
		off = base;
	if (off + npages > end) {
		mutex_unlock(&reg_lock);
		pr_warn_ratelimited("sev_gpu: osdesc carve full VM%u off=0x%llx need=0x%llx end=0x%llx\n",
				    client_id, (unsigned long long)off,
				    (unsigned long long)npages,
				    (unsigned long long)end);
		return -ENOSPC;
	}
	osdesc_carve_cursor[client_id] = off + npages;
	if (npages >= 0x200000) {
		WRITE_ONCE(osdesc_2m_off[client_id], off);
		if (!READ_ONCE(userd_2m_off[client_id])) {
			WRITE_ONCE(userd_2m_off[client_id], off);
			// pr_info("sev_gpu: DIAG armed USERD/GPFIFO probe VM%u off=0x%llx (ring + USERD GP_PUT sampled after each replay)\n",
			// 	client_id, (unsigned long long)off);
		}
		/*
		 * Record this >=2 MiB carve as a shared-USERD candidate for the
		 * GP_PUT-advance doorbell ring (the CE/engine-0x31 channel places
		 * its GPFIFO at buf+0 and USERD at buf+0x2000 inside such a carve).
		 */
		{
			u32 nc = READ_ONCE(bringup_ncand[client_id]);

			if (nc < SEV_GPU_BRINGUP_MAX_CAND) {
				bringup_userd_cand[client_id][nc] = off;
				bringup_cand_lastput[client_id][nc] = 0;
				bringup_cand_lastget[client_id][nc] = 0;
				WRITE_ONCE(bringup_ncand[client_id], nc + 1);
			}
		}
		// pr_info("sev_gpu: DIAG armed 0x3e tail probe VM%u off=0x%llx (word@+0x3fffc will be sampled after each replay)\n",
		// 	client_id, (unsigned long long)off);
	}
	mutex_unlock(&reg_lock);

	/*
	 * Zero the freshly-carved slice. The bump pool is NOT cleared on reset,
	 * so a reused offset still holds a PRIOR client run's bytes -- including
	 * a stale USERD GP_PUT and its GPFIFO entries. The GP_PUT-advance doorbell
	 * watcher would then ring on that stale value before CUDA has even built
	 * the channel that owns the buffer, making the GPU execute garbage GPFIFO
	 * entries and fault (Xid 45 RC_TRIGGERED -> NV_ERR_RESET_REQUIRED). Zeroing
	 * on carve makes GP_PUT/GP_GET/GPFIFO start at 0 (as RM expects for a fresh
	 * USERD) so the watcher only fires on a genuine 0->N advance post-construct.
	 */
	if (dd->mem && off + npages <= (u64)dd->mem_size)
		memset_io((u8 __iomem *)dd->mem + off, 0, npages);

	*mgr_phys = dd->mem_phys + off;
	*cli_phys = client_phys + off;
	pr_info("sev_gpu: osdesc carve VM%u off=0x%llx size=0x%llx mgr=0x%llx cli=0x%llx\n",
		client_id, (unsigned long long)off, (unsigned long long)npages,
		(unsigned long long)*mgr_phys, (unsigned long long)*cli_phys);
	return 0;
}

/*
 * Manager: reclaim client @client_id's entire OS-descriptor / shared-sysmem
 * carve pool by rewinding its bump cursor to the reserve base.  nvidia.ko calls
 * this (via the registered osdesc-reset hook) only after the last live carve
 * for the client has been freed, so no in-use slice is ever clobbered.  Without
 * it the monotonic cursor leaks the per-run working set (~2 MiB) and the fixed
 * reserve is exhausted after a couple of CUDA runs -- the 0x3e carve then fails
 * -ENOSPC and aborts the next cudaMalloc.
 */
static void sev_gpu_osdesc_reset_impl(u32 client_id)
{
	if ((u32)client_id >= SEV_GPU_MAX_VMS)
		return;

	mutex_lock(&reg_lock);
	osdesc_carve_cursor[client_id] = 0;
	WRITE_ONCE(userd_2m_off[client_id], 0);
	mutex_unlock(&reg_lock);

	pr_info("sev_gpu: osdesc carve pool reclaimed VM%u\n", client_id);
}

/*
 * Manager: translate a manager-view guest-phys inside any client's ivshmem data
 * region to the kernel VA of our existing coherent mapping of that region
 * (dd->mem + off).  nvidia.ko calls this when RM needs a CPU kernel mapping of
 * an OS_PHYS_ADDR object carved from ivshmem (e.g. a channel error-notifier):
 * those BAR pages have no valid struct page, so vmap() there fails.  Returns 0
 * if [phys, phys+size) is not fully contained in one mapped data region.
 */
static unsigned long sev_gpu_shared_phys_to_va_impl(u64 phys, u64 size)
{
	u32 i;

	if (!size)
		size = PAGE_SIZE;

	for (i = 0; i < (u32)num_data_devs && i < SEV_GPU_MAX_VMS; i++) {
		struct sev_gpu_data_dev *dd = data_devs[i];

		if (!dd || !dd->mem || !dd->mem_size)
			continue;
		if (phys >= dd->mem_phys &&
		    phys + size <= dd->mem_phys + dd->mem_size) {
			u64 off = phys - dd->mem_phys;

			return (unsigned long)(uintptr_t)((u8 __force *)dd->mem + off);
		}
	}
	return 0;
}

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

/* Manager: service one VM's control-BAR mailbox that holds a pending request. */
static void rpc_service_slot(u8 vm, sev_gpu_rpc_slot_t *slot)
{
	void __iomem *mb = rpc_ctrl_mailbox(vm);
	struct sev_gpu_data_dev *dd;
	sev_gpu_rm_replay_t replay;
	u64 stage_base = 0;
	u32 size, n, i;
	bool nested_ok = true;

	/* The slot (header + descriptors + top-level arg) must fit one slot. */
	BUILD_BUG_ON(sizeof(sev_gpu_rpc_slot_t) > SEV_GPU_RPC_SLOT_STRIDE);

	if (!mb)
		return;

	memcpy_fromio(slot, mb, sizeof(*slot));
	size = (slot->arg_size <= SEV_GPU_RPC_INLINE_MAX) ?
			slot->arg_size : SEV_GPU_RPC_INLINE_MAX;

	/* Cache the client's data-region GPA for sev_gpu_shadow_db_impl().
	 * This is the primary path: the RPC slot lives in the control BAR whose
	 * writes are always visible to the manager regardless of data-device
	 * presence or SEV-SNP C-bit state on the client side. */
	if (slot->client_data_phys && vm < SEV_GPU_MAX_VMS)
		WRITE_ONCE(client_mem_phys_cache[vm], slot->client_data_phys);

	/* Our mapping of this VM's data region holds the staged pointees. */
	dd = ((u32)vm < (u32)num_data_devs) ? data_devs[vm] : NULL;
	if (dd && dd->mem)
		stage_base = rpc_staging_base(dd->mem_size);

	/*
	 * Zero-copy nested params: re-point each pointer the client staged into
	 * its data region at the kernel address of OUR mapping of that same
	 * region (dd->mem + staging_off). The RM runs this client's replay with
	 * PARAM_LOCATION_KERNEL, so it reads (IN) and writes (OUT) the pointee
	 * directly in the shared region -- the manager copies nothing. This path
	 * is cmd-agnostic: it trusts only the client-provided buffers[] and
	 * bounds-checks every offset against the data region it already maps.
	 *
	 * NOTE: dd->mem is an ioremap'd (noncached, decrypted) __iomem mapping of
	 * BAR2. On x86 the RM's CPU memcpy/field access to this address works (it
	 * is just uncached); if a future RM path needs a cached, normal-pointer
	 * view, swap dd->mem here for a memremap(WB|DEC) mapping of dd->mem_phys.
	 */
	n = slot->n_buffers;
	if (n > SEV_GPU_RPC_MAX_BUFFERS || (n > 0 && (!dd || !dd->mem || !stage_base))) {
		n = 0;
		nested_ok = false;
	}
	/*
	 * Pass A -- top-level pointers: re-point each NvP64 field inside the
	 * inline top-level arg at the kernel address of OUR mapping of the staged
	 * pointee (dd->mem + staging_off).
	 */
	for (i = 0; nested_ok && i < n; i++) {
		sev_gpu_rpc_buffer_t *b = &slot->buffers[i];
		u64 kva;

		if (b->parent != SEV_GPU_RPC_PARENT_TOPLEVEL)
			continue;
		if (b->struct_off + 8 > size ||
		    b->staging_off < stage_base ||
		    b->size > SEV_GPU_RPC_DATA_STAGING_SIZE ||
		    b->staging_off + b->size > dd->mem_size) {
			nested_ok = false;
			break;
		}
		kva = (u64)(uintptr_t)((u8 __force *)dd->mem + b->staging_off);
		memcpy(slot->inline_arg + b->struct_off, &kva, 8);
	}
	/*
	 * Pass B -- level-2 pointers: each embedded NvP64 field lives @struct_off
	 * INSIDE a parent staged buffer (its pParams), so patch it in place within
	 * dd->mem rather than in the inline arg. Single nesting level only: the
	 * parent must itself be a top-level staged buffer.
	 */
	for (i = 0; nested_ok && i < n; i++) {
		sev_gpu_rpc_buffer_t *b = &slot->buffers[i];
		sev_gpu_rpc_buffer_t *p;
		u64 kva;

		if (b->parent == SEV_GPU_RPC_PARENT_TOPLEVEL)
			continue;
		if (b->parent >= n) {
			nested_ok = false;
			break;
		}
		p = &slot->buffers[b->parent];
		if (p->parent != SEV_GPU_RPC_PARENT_TOPLEVEL ||
		    b->struct_off + 8 > p->size ||
		    b->staging_off < stage_base ||
		    b->size > SEV_GPU_RPC_DATA_STAGING_SIZE ||
		    b->staging_off + b->size > dd->mem_size ||
		    p->staging_off < stage_base ||
		    p->staging_off + p->size > dd->mem_size) {
			nested_ok = false;
			break;
		}
		kva = (u64)(uintptr_t)((u8 __force *)dd->mem + b->staging_off);
		memcpy_toio((u8 __iomem *)dd->mem + p->staging_off + b->struct_off,
			    &kva, 8);
	}

	replay = READ_ONCE(rpc_replay_fn);
	if (!nested_ok) {
		slot->rm_status = (s32)RPC_FWD_ERR;
		pr_warn_ratelimited("sev_gpu: rpc bad nested table vm=%u cmd=0x%x n=%u\n",
				    vm, slot->cmd, slot->n_buffers);
	} else if (rpc_loopback || !replay) {
		/* Transport test: leave inline_arg unchanged, report success. */
		slot->rm_status = 0;
	} else {
		slot->rm_status = replay(vm, slot->cmd, slot->inline_arg, size);
	}
	slot->ret = 0;

	/*
	 * Scrub the patched kernel VAs back to their staging offsets so we never
	 * publish kernel addresses into shared memory. Top-level pointers live in
	 * the inline arg (republished in the slot copy); level-2 pointers live in
	 * the parent's staged buffer within dd->mem. Any OUT pointee the RM wrote
	 * already lives in the client's data region (in place); the client reads
	 * it back from there, so nothing extra rides in the slot copy.
	 */
	for (i = 0; nested_ok && i < n; i++) {
		sev_gpu_rpc_buffer_t *b = &slot->buffers[i];

		if (b->parent == SEV_GPU_RPC_PARENT_TOPLEVEL) {
			memcpy(slot->inline_arg + b->struct_off, &b->staging_off, 8);
		} else {
			sev_gpu_rpc_buffer_t *p = &slot->buffers[b->parent];

			memcpy_toio((u8 __iomem *)dd->mem + p->staging_off + b->struct_off,
				    &b->staging_off, 8);
		}
	}

	/* Write the reply back, then publish REPLY last. */
	memcpy_toio(mb, slot, sizeof(*slot));
	wmb();
	iowrite32(SEV_GPU_RPC_REPLY, mb + RPC_STATE_OFF);

	/*
	 * Bring-up doorbell watch (Option C): a replay-allocated GPFIFO channel
	 * (esc 0x2b RM_ALLOC, hClass 0x..6f in the 0xc3..0xc9 architecture range)
	 * is created under the client's own (hRoot@0, hObjectNew@8) in the
	 * manager's replay namespace, so those handles ring the real doorbell.
	 * Arm a watch on the shadow doorbell page: unmodified CUDA's bring-up ring
	 * is a plain memory write the manager receives no interrupt for, so the
	 * replay poller samples it and rings on an advance. Arm only on success.
	 */
	if (slot->cmd == RPC_NV_ESC_RM_ALLOC && size >= 16 && slot->rm_status == 0) {
		u32 hClass = *(const u32 *)(slot->inline_arg + 12);

		if ((hClass & 0xffu) == 0x6fu &&
		    (hClass >> 8) >= 0xc3u && (hClass >> 8) <= 0xc9u) {
			u32 hRoot = *(const u32 *)(slot->inline_arg + 0);
			u32 hChan = *(const u32 *)(slot->inline_arg + 8);

			sev_gpu_bringup_arm(vm, hRoot, hChan);
		}
	}

	/*
	 * DIAG (read-only): log channel-GPFIFO control replays during bring-up.
	 * An NV_ESC_RM_CONTROL (0x2a) escape carries an NVOS54_PARAMETERS in the
	 * inline arg: hObject @+4, inner control cmd @+8, NVOS54.status @+28.
	 * Channel-GPFIFO controls have interface id 0x..6f in the high 16 bits
	 * (0xa06f GPFIFO_SCHEDULE=0xa06f0103; 0xc36f GET_WORK_SUBMIT_TOKEN=
	 * 0xc36f0108, SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX=0xc36f010a; RESET_CHANNEL).
	 * This shows whether CUDA schedules the channel and fetches its
	 * work-submit token -- the token it MUST obtain before it can ever write
	 * NVC361_NOTIFY_CHANNEL_PENDING (+0x90) to ring the doorbell -- and
	 * whether each replay succeeds (rm_status / NVOS54.status). Pure read.
	 */
	if (slot->cmd == 0x2a && size >= 32) {
		u32 inner = *(const u32 *)(slot->inline_arg + 8);

		if (((inner >> 16) & 0xffu) == 0x6fu) {
			u32 hobj = *(const u32 *)(slot->inline_arg + 4);
			u32 nstat = *(const u32 *)(slot->inline_arg + 28);

			pr_debug("sev_gpu: DIAG fifo-ctrl vm=%u hObject=0x%08x cmd=0x%08x rm_status=%d nvos54.status=0x%08x\n",
				 vm, hobj, inner, (int)slot->rm_status, nstat);
		}
	}

	/*
	 * DIAG (read-only): flag any replay that returns an error, to pinpoint
	 * the fast bring-up abort. CUDA gets its work-submit token then tears the
	 * channel down ~0.4s later without ever scheduling or ringing -- that is
	 * an error abort, not a poll timeout. Log the transport status (rm_status)
	 * and, for RM_CONTROL (0x2a) escapes, the inner NVOS54 cmd (@+8) and its
	 * per-control status (@+28). Low-noise: only non-zero statuses print.
	 */
	{
		u32 inner = (slot->cmd == 0x2a && size >= 12) ?
			    *(const u32 *)(slot->inline_arg + 8) : 0;
		u32 nstat = (slot->cmd == 0x2a && size >= 32) ?
			    *(const u32 *)(slot->inline_arg + 28) : 0;

		if (slot->rm_status != 0 || nstat != 0)
			pr_debug("sev_gpu: DIAG FAIL vm=%u cmd=0x%x inner=0x%08x rm_status=%d nvos54.status=0x%08x\n",
				 vm, slot->cmd, inner, (int)slot->rm_status, nstat);
	}

	/*
	 * DIAG (read-only): NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST (0x0080170d) is
	 * the UVM channel bring-up control that fails with NV_ERR_INVALID_ARGUMENT
	 * (0x1f). Its RM handler rejects ONLY numChannels==0, and its pParams
	 * embeds two count-sized pointers -- pChannelHandleList @+8 (IN) and
	 * pChannelList @+16 (OUT) -- that are NOT in the level-2 marshal policy
	 * (rpc_ctrl_policies[]), so the manager never re-points them. Dump the
	 * NVOS54 flags (FINN? which transport) + paramsSize, and, when pParams was
	 * staged (post-scrub the params field @+16 holds a data-region offset),
	 * the staged numChannels @+0 and both embedded pointer fields, to see
	 * exactly what the manager RM reads. Pure read; alters no state.
	 */
	if (slot->cmd == 0x2a && size >= 32 &&
	    *(const u32 *)(slot->inline_arg + 8) == 0x0080170du) {
		u32 flags = *(const u32 *)(slot->inline_arg + 12);
		u32 psz   = *(const u32 *)(slot->inline_arg + 24);
		u64 poff  = *(const u64 *)(slot->inline_arg + 16);
		u32 nch = 0;
		u64 p1 = 0, p2 = 0;
		bool staged = dd && dd->mem && stage_base &&
			      poff >= stage_base && poff + 24 <= (u64)dd->mem_size &&
			      !(flags & RPC_NVOS54_FLAGS_FINN_SERIALIZED);

		if (staged) {
			nch = ioread32((u8 __iomem *)dd->mem + poff);
			memcpy_fromio(&p1, (u8 __iomem *)dd->mem + poff + 8, 8);
			memcpy_fromio(&p2, (u8 __iomem *)dd->mem + poff + 16, 8);
		}
		pr_debug("sev_gpu: DIAG chlist vm=%u flags=0x%x paramsSize=%u params_off=0x%llx staged=%d numChannels=%u hList=0x%llx cList=0x%llx nvos54.status=0x%08x\n",
			 vm, flags, psz, (unsigned long long)poff, staged, nch,
			 (unsigned long long)p1, (unsigned long long)p2,
			 *(const u32 *)(slot->inline_arg + 28));
	}

	/*
	 * DIAG (read-only): after each replay on the real servicing path, sample
	 * the manager's own copy of the "0x3e" compute-pool control word at
	 * pool+0x3fffc that CUDA faults on in the client. Non-zero here while the
	 * client reads 0 => a link/coherency bug; still 0 after the whole channel
	 * construct => the value is produced by GPU channel bring-up the manager
	 * never triggers. Pure read; alters no state.
	 */
	if (dd && dd->mem && vm < SEV_GPU_MAX_VMS) {
		u64 o2 = READ_ONCE(osdesc_2m_off[vm]);
		u64 db = compute_doorbell_off(dd->mem_size);

		if (o2 && o2 + 0x40000 <= (u64)dd->mem_size) {
			u32 w0 = ioread32((u8 __iomem *)dd->mem + o2 + 0x3fffc);
			u32 wa = ioread32((u8 __iomem *)dd->mem + o2 + 0x3ffc0);
			u32 wb = ioread32((u8 __iomem *)dd->mem + o2 + 0x3ffd0);

			pr_debug("sev_gpu: DIAG 0x3e vm=%u off=0x%llx word[0x3fffc]=0x%08x [0x3ffc0]=0x%08x [0x3ffd0]=0x%08x\n",
				 vm, (unsigned long long)o2, w0, wa, wb);
		}

		/*
		 * DIAG (read-only): sample specific fields of the client's shadow
		 * usermode page rather than "first non-zero" -- the page's TIME_0
		 * field (NV_USERMODE_TIME_0, page offset 0x80) mirrors a live ~1GHz
		 * nanosecond clock, so a first-non-zero scan always stops there and
		 * never reaches the actual doorbell. The work-submit doorbell is
		 * NVC361_NOTIFY_CHANNEL_PENDING at page offset 0x90 (see
		 * nv_gpu_ops.c: workSubmissionOffset = clientRegionMapping + 0x90).
		 * If CUDA rings its (redirected) usermode doorbell during bring-up,
		 * its token lands at db+0x90 in the shared ivshmem page -- yet the
		 * manager (sole real-doorbell ringer) only propagates it on an
		 * explicit STAGED flush. A non-zero token here while word[0x3fffc]
		 * stays 0 proves the ring is dropped, not missing. Pure read.
		 */
		if (db && db + 0xA0 <= (u64)dd->mem_size) {
			u32 t0 = ioread32((u8 __iomem *)dd->mem + db + 0x80);
			u32 t1 = ioread32((u8 __iomem *)dd->mem + db + 0x84);
			u32 d0 = ioread32((u8 __iomem *)dd->mem + db + 0x90);
			u32 d1 = ioread32((u8 __iomem *)dd->mem + db + 0x94);
			u32 d2 = ioread32((u8 __iomem *)dd->mem + db + 0x98);
			u32 d3 = ioread32((u8 __iomem *)dd->mem + db + 0x9c);

			pr_debug("sev_gpu: DIAG shadow-db vm=%u off=0x%llx TIME[0x80]=0x%08x[0x84]=0x%08x DOORBELL[0x90]=0x%08x[0x94]=0x%08x[0x98]=0x%08x[0x9c]=0x%08x\n",
				 vm, (unsigned long long)db, t0, t1, d0, d1, d2, d3);
		}

		/*
		 * DIAG (read-only): the channel USERD/GPFIFO ring is now backed by
		 * shared ivshmem (Task A), so the manager can see whether CUDA ever
		 * attempts a submit. The buffer holds the GPFIFO ring at offset 0
		 * (gpFifoOffset) and the USERD control struct at userdOff 0x2000
		 * (KeplerBControlGPFifo: Put@0x40, Get@0x44, GPGet@0x88, GPPut@0x8c).
		 * A non-zero ring entry / GPPut proves CUDA wrote a submission into
		 * the shared ring; with DOORBELL[0x90]==0 that means it was written
		 * but never rung (manager must propagate it). All zero through
		 * teardown => CUDA never submits on this channel. Pure read.
		 */
		{
			u64 uo = READ_ONCE(userd_2m_off[vm]);

			if (uo && uo + 0x2090 <= (u64)dd->mem_size) {
				u32 r0 = ioread32((u8 __iomem *)dd->mem + uo + 0x00);
				u32 r1 = ioread32((u8 __iomem *)dd->mem + uo + 0x04);
				u32 r2 = ioread32((u8 __iomem *)dd->mem + uo + 0x08);
				u32 r3 = ioread32((u8 __iomem *)dd->mem + uo + 0x0c);
				u32 uput  = ioread32((u8 __iomem *)dd->mem + uo + 0x2040);
				u32 uget  = ioread32((u8 __iomem *)dd->mem + uo + 0x2044);
				u32 gpget = ioread32((u8 __iomem *)dd->mem + uo + 0x2088);
				u32 gpput = ioread32((u8 __iomem *)dd->mem + uo + 0x208c);

				pr_debug("sev_gpu: DIAG userd vm=%u off=0x%llx ring[0]=0x%08x [1]=0x%08x [2]=0x%08x [3]=0x%08x GPPut=0x%08x GPGet=0x%08x Put=0x%08x Get=0x%08x\n",
					 vm, (unsigned long long)uo, r0, r1, r2, r3,
					 gpput, gpget, uput, uget);
			}
		}
	}

	/* Best-effort wake of the client (it also polls). The client's slot id
	 * equals its ivshmem peer id, so ring that peer. */
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, (u16)vm, IVSHMEM_VECTOR_RPC);
}

/*
 * Manager replay poller. Scans every VM's control-BAR mailbox slot for pending
 * requests, services them, and otherwise sleeps until the RPC doorbell kicks it
 * or a short idle-poll timeout elapses (closes the kick-after-scan race, and
 * makes progress even if the doorbell lands on the wrong peer id).
 */
static int rpc_thread_fn(void *unused)
{
	sev_gpu_rpc_slot_t *slot;

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		bool did = false;
		bool watching;
		int vm;

		for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
			void __iomem *mb = rpc_ctrl_mailbox((u8)vm);

			if (!mb)
				continue;
			if (ioread32(mb + RPC_STATE_OFF) == SEV_GPU_RPC_REQUEST) {
				rpc_service_slot((u8)vm, slot);
				did = true;
			}
		}

		/* Service pending in-kernel handshake requests (manager side). */
		if (auto_mtls && ctrl_dev && ctrl_dev->is_manager) {
			for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
				void __iomem *hb = hs_ctrl_mailbox((u8)vm);

				if (!hb)
					continue;
				if (ioread32(hb + offsetof(sev_gpu_hs_slot_t,
							  state)) ==
				    SEV_GPU_HS_REQ) {
					sev_gpu_hs_service_slot((u8)vm, hb);
					did = true;
				}
			}
		}

		/*
		 * Propagate any bring-up doorbell the client's CUDA rang into the
		 * shadow ivshmem page (a plain memory write raises no MSI-X, so it
		 * must be polled). While a watch is armed, sample finely.
		 */
		watching = sev_gpu_bringup_poll();

		if (!did)
			wait_event_interruptible_timeout(rpc_wq,
				atomic_xchg(&rpc_kick, 0) || kthread_should_stop(),
				msecs_to_jiffies(watching ? SEV_GPU_BRINGUP_POLL_MS
							  : RPC_IDLE_POLL_MS));
	}

	kfree(slot);
	return 0;
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
typedef void (*sev_gpu_register_mmap_redirect_t)(
	int (*fn)(u64 phys, u64 size, unsigned long *pfn_out));
typedef void (*sev_gpu_unregister_mmap_redirect_t)(void);

static sev_gpu_unregister_mmap_redirect_t mmap_unregister_fn;

extern void sev_gpu_register_mmap_redirect(
	int (*fn)(u64 phys, u64 size, unsigned long *pfn_out));
extern void sev_gpu_unregister_mmap_redirect(void);

static int sev_gpu_mmap_redirect_impl(u64 phys, u64 size, unsigned long *pfn_out)
{
	struct sev_gpu_data_dev *dd;
	u64 reserve_base, reserve_end;

	if (!size || !IS_ALIGNED(phys, PAGE_SIZE))
		return -ENOENT;

	/*
	 * phys == 0: libcuda.so uses mmap(fd, offset=0) -- the normal NVIDIA
	 * convention where the kernel mmap context (stored by
	 * rm_create_mmap_context) carries the physical address.  We never call
	 * rm_create_mmap_context on the client, so we stash the shadow doorbell
	 * PFN in doorbell_mmap_pfn when the MAP_MEMORY reply arrives and serve
	 * it here as a one-shot.
	 */
	if (!phys) {
		unsigned long pfn = READ_ONCE(doorbell_mmap_pfn);
		if (pfn) {
			WRITE_ONCE(doorbell_mmap_pfn, 0);
			*pfn_out = pfn;
			pr_info("sev_gpu: mmap_redirect doorbell pfn=0x%lx\n", pfn);
			return 0;
		}
		pr_warn("sev_gpu: mmap_redirect: mmap offset=0 but no doorbell pfn stored\n");
		return -ENOENT;
	}

	dd = data_devs[0];
	if (!dd || !dd->mem_phys || !dd->mem_size)
		return -ENOENT;

	reserve_base = (u64)dd->mem_phys + compute_reserve_base(dd->mem_size);
	if (!reserve_base)
		return -ENOENT;
	reserve_end = reserve_base + SEV_GPU_COMPUTE_RESERVE_SIZE;

	pr_info("sev_gpu: mmap_redirect probe phys=0x%llx size=0x%llx"
		" reserve=[0x%llx,0x%llx)\n",
		(unsigned long long)phys, (unsigned long long)size,
		(unsigned long long)reserve_base, (unsigned long long)reserve_end);

	if (phys < reserve_base || phys + size > reserve_end) {
		pr_warn("sev_gpu: mmap_redirect MISS phys=0x%llx mem_phys=0x%llx"
			" reserve_base=0x%llx\n",
			(unsigned long long)phys,
			(unsigned long long)dd->mem_phys,
			(unsigned long long)reserve_base);
		return -ENOENT;
	}

	*pfn_out = phys >> PAGE_SHIFT;
	pr_info("sev_gpu: mmap redirect phys=0x%llx size=0x%llx pfn=0x%lx\n",
		(unsigned long long)phys, (unsigned long long)size, *pfn_out);
	return 0;
}

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
#define SEV_GPU_USERMODE_TIME_0_OFF	0x80u	/* NVC361_TIME_0 */
#define SEV_GPU_USERMODE_TIME_1_OFF	0x84u	/* NVC361_TIME_1 */
#define SEV_GPU_USERMODE_TIME_0_MASK	0xffffffe0u /* NV_PTIMER_TIME_0_NSEC 31:5 */
#define SEV_GPU_USERMODE_TIMER_PERIOD_NS	(20u * NSEC_PER_USEC)

static struct hrtimer usermode_timer;
static bool usermode_timer_armed;

static void sev_gpu_usermode_timer_write(void)
{
	struct sev_gpu_data_dev *dd = data_devs[0];
	u8 __iomem *page;
	u64 off, ns;

	if (!dd || !dd->mem || !dd->mem_size)
		return;
	off = compute_doorbell_off(dd->mem_size);
	if (!off)
		return;

	ns = ktime_get_ns();
	page = (u8 __iomem *)dd->mem + off;

	/*
	 * Write TIME_1 (hi) before TIME_0 (lo).  The reader loop reads hi, lo,
	 * hi again and retries while the two hi reads disagree; publishing hi
	 * first means a reader that interleaves our two stores never
	 * reconstructs a low word that has wrapped past a hi word we have not
	 * yet bumped -- so it can never observe a backward jump at the ~4.29 s
	 * lo-rollover boundary.  Ordered by the UC ivshmem mapping.
	 */
	iowrite32((u32)(ns >> 32), page + SEV_GPU_USERMODE_TIME_1_OFF);
	iowrite32((u32)ns & SEV_GPU_USERMODE_TIME_0_MASK,
		  page + SEV_GPU_USERMODE_TIME_0_OFF);
}

static enum hrtimer_restart sev_gpu_usermode_timer_fn(struct hrtimer *t)
{
	sev_gpu_usermode_timer_write();
	hrtimer_forward_now(t, ns_to_ktime(SEV_GPU_USERMODE_TIMER_PERIOD_NS));
	return HRTIMER_RESTART;
}

static void sev_gpu_usermode_timer_start(void)
{
	if (usermode_timer_armed)
		return;
	hrtimer_setup(&usermode_timer, sev_gpu_usermode_timer_fn,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	usermode_timer_armed = true;
	hrtimer_start(&usermode_timer,
		      ns_to_ktime(SEV_GPU_USERMODE_TIMER_PERIOD_NS),
		      HRTIMER_MODE_REL);
	pr_info("sev_gpu: USERMODE shadow GPU-timer emulation armed (period %u ns)\n",
		(unsigned int)SEV_GPU_USERMODE_TIMER_PERIOD_NS);
}

static void sev_gpu_usermode_timer_stop(void)
{
	if (!usermode_timer_armed)
		return;
	hrtimer_cancel(&usermode_timer);
	usermode_timer_armed = false;
}

static void mmap_client_bind_nvidia(void)
{
	sev_gpu_register_mmap_redirect_t reg;

	reg = symbol_get(sev_gpu_register_mmap_redirect);
	if (!reg) {
		pr_info("sev_gpu: nvidia mmap redirect absent; USERD mmap will fail until bound\n");
		return;
	}
	mmap_unregister_fn = symbol_get(sev_gpu_unregister_mmap_redirect);
	reg(sev_gpu_mmap_redirect_impl);
	symbol_put(sev_gpu_register_mmap_redirect);
	pr_info("sev_gpu: registered mmap redirect with nvidia.ko\n");
}

static void mmap_client_unbind_nvidia(void)
{
	if (mmap_unregister_fn) {
		mmap_unregister_fn();
		symbol_put(sev_gpu_unregister_mmap_redirect);
		mmap_unregister_fn = NULL;
	}
}

/*
 * Client: optionally bind our forwarder into nvidia.ko's escape hook. Done via
 * symbol_get so this module stays loadable when nvidia.ko is absent (e.g. on an
 * ordinary VM used for transport bring-up).
 */
extern void sev_gpu_register_rm_forwarder(sev_gpu_rm_forwarder_t fn);
extern void sev_gpu_unregister_rm_forwarder(void);

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
static void sev_gpu_client_attach_gpu(struct sev_gpu_dev *d)
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
