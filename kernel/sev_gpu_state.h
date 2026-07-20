/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_state.h — shared module state (named types + extern globals).
 *
 * The manager's secure-channel subsystem (kmb, handshake, assign, exec, sched)
 * shares seven module-global state objects. To split those clusters into
 * separate .c files, the anonymous `static struct {} name;` globals are given
 * NAMED types here and extern-declared; they are DEFINED once in sev_gpu_main.c
 * (formerly sev_gpu_manager.c).
 *
 * Record structs referenced by the globals also live here so any file can use
 * them.
 */
#ifndef SEV_GPU_STATE_H
#define SEV_GPU_STATE_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include "sev_gpu_transport.h"  /* IVSHMEM_NUM_VECTORS */
#include "sev_gpu_manager.h"   /* SEV_GPU_MAX_VMS, MAX_CHANNELS_PER_VM, CC_POOL_MAX,
                                  COMM_KEY_LEN, sev_cc_kmb, HS_CONFIRM_LEN */
#include "sev_gpu_crypto.h"    /* SEV_GPU_HS_CONFIRM_LEN */

/* ---- record structs (were defined inline in the .c) ---- */
struct sev_gpu_assignment {
	bool   in_use;
	u32    kind;                 /* SEV_GPU_CHAN_KIND_*                  */
	u32    channel_id;
	u32    keyspace;
	u32    h_client;
	u32    h_channel;
	u32    generation;
	u64    userd_off;
	u64    gpfifo_off;
	u64    pushbuf_off;
	u64    pushbuf_gpu_va;
	u64    enc_off;
	u32    gp_put;
	struct sev_cc_kmb kmb;
};

struct sev_gpu_cc_chan {
	bool provisioned;
	bool in_use;
	u32  h_client;
	u32  h_channel;
	u32  keyspace;
	u32  channel_id;
	u8   owner_vm;
};

struct sev_client_chan {
	bool valid;
	u32  channel_id;
	u32  keyspace;
	u32  generation;
	struct sev_cc_kmb kmb;
	u64  ctr_h2d;
	u64  ctr_d2h;
};

/* ---- named global-state types ---- */
struct sev_gpu_manager_state {
	spinlock_t lock;
	u8 num_vms;
	unsigned long registered;
	u8 gpu_owner;
};

struct sev_gpu_comm_keystore {
	spinlock_t lock;
	u8 key[SEV_GPU_MAX_VMS][SEV_GPU_COMM_KEY_LEN];
	unsigned long valid;
};

struct sev_gpu_assign_state {
	spinlock_t lock;
	struct sev_gpu_assignment a[SEV_GPU_MAX_VMS][SEV_GPU_MAX_CHANNELS_PER_VM];
};

struct sev_gpu_cc_pool {
	spinlock_t lock;
	struct sev_gpu_cc_chan e[SEV_GPU_CC_POOL_MAX];
};

struct sev_gpu_client_kmb_store {
	spinlock_t lock;
	struct sev_client_chan c[SEV_GPU_MAX_CHANNELS_PER_VM];
};

struct sev_gpu_hs_state {
	struct workqueue_struct *wq;
	struct work_struct       work;
	spinlock_t               lock;
	unsigned long            pending;
};

struct sev_gpu_hs_mgr_state {
	bool active;
	u8   comm_key[SEV_GPU_COMM_KEY_LEN];
	u8   confirm_key[SEV_GPU_HS_CONFIRM_LEN];
	u8   th[32];
};

/* ---- extern globals (defined in sev_gpu_main.c) ---- */
extern struct sev_gpu_manager_state    manager_state;
extern struct sev_gpu_comm_keystore    comm_keystore;
extern struct sev_gpu_assign_state     assign_state;
extern struct sev_gpu_cc_pool          cc_pool;
extern struct sev_gpu_client_kmb_store client_kmb_store;
extern struct sev_gpu_hs_state         hs_state;
extern struct sev_gpu_hs_mgr_state     hs_mgr_state[SEV_GPU_MAX_VMS];


/* ---- core device structs (were inline in the .c) ---- */
struct sev_gpu_irq {
	struct sev_gpu_dev *dev;
	int vector;
};

struct sev_gpu_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	/*
	 * BAR2 shared region. Under the unified sev-channel device (Topology 1)
	 * ONE pci_dev carries both signalling (BAR0=regs) and the per-client data
	 * region (BAR2). The data-view field names (mem/mem_phys/mem_size) are
	 * UNION ALIASES of the signalling names (shmem/shmem_phys/shmem_size) so
	 * the ~228 existing dd->mem* references compile unchanged after
	 * sev_gpu_data_dev is typedef'd to this struct.
	 */
	union { void __iomem *shmem;      void __iomem *mem;      };
	union { phys_addr_t   shmem_phys; phys_addr_t   mem_phys; };
	union { size_t        shmem_size; size_t        mem_size; };
	int  ivposition;
	bool is_manager;
	u8   client_vm_id;
	u8   comm_vm_id;
	u32  pool_index;              /* manager slot (probe order); was data_dev */
	u32  last_db_seq;             /* independent doorbell path: last db_seq the
	                              * manager serviced for this client region. A
	                              * mismatch vs header db_seq == new compute
	                              * doorbell to mirror. */
	int  nvectors;
	struct sev_gpu_irq irqctx[IVSHMEM_NUM_VECTORS];
	u64 request_off;
	u64 grant_off;
	u64 data_off;
	u32 per_vm_data;
	wait_queue_head_t grant_wq;
	struct workqueue_struct *sched_wq;
	struct work_struct sched_work;
	struct workqueue_struct *rpc_wq;
	struct work_struct rpc_work;
	struct mutex rpc_lock;
	atomic_t rpc_seq;
	struct mutex copy_lock;
	atomic_t mgr_polling;
	atomic_t cli_polling;
	/* Control chardev (/dev/sev_gpu_manager): ioctl interface. */
	dev_t devt;
	struct cdev cdev;
	struct device *device;
	/*
	 * Data chardev (/dev/sev_gpu_dataN): BAR2 mmap interface. Kept SEPARATE
	 * from the control chardev to preserve the NVIDIA ctl-vs-device node
	 * distinction (control maps sysmem params; device maps the BAR/doorbell).
	 */
	dev_t         data_devt;
	struct cdev   data_cdev;
	struct device *data_device;
};

/*
 * Unified device: the former per-VM DATA device is now the SAME struct as the
 * control/signalling device (one sev-channel pci_dev per client carries both).
 * `struct sev_gpu_data_dev` resolves to `struct sev_gpu_dev` via this macro, so
 * all existing `struct sev_gpu_data_dev` references compile unchanged. The few
 * bare-typedef uses were converted to the struct form.
 */
#define sev_gpu_data_dev sev_gpu_dev

/* The one control device (defined in sev_gpu_main.c). */
extern struct sev_gpu_dev *ctrl_dev;

/* PRIVATE per-VM DATA device (ivshmem-plain BAR2). */
/* PRIVATE per-VM DATA device — now unified with sev_gpu_dev (see typedef above).
 * data_devs[] holds the SAME device instances as the signalling side; under the
 * channel a single pci_dev is both. Kept as a distinct array + accessor name so
 * the ~228 existing data-path references are unchanged. */
extern struct sev_gpu_data_dev *data_devs[SEV_GPU_MAX_VMS];

/* client-side + bring-up shared state (defined in sev_gpu_main.c). */
extern int num_data_devs;
extern u64 client_mem_phys_cache[SEV_GPU_MAX_VMS];
extern struct mutex reg_lock;
extern u64 osdesc_carve_cursor[SEV_GPU_MAX_VMS];
extern u64 osdesc_2m_off[SEV_GPU_MAX_VMS];
extern u64 userd_2m_off[SEV_GPU_MAX_VMS];
extern u32 bringup_ncand[SEV_GPU_MAX_VMS];
extern u64 bringup_userd_cand[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
extern u32 bringup_cand_lastput[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];
extern u32 bringup_cand_lastget[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CAND];

/* manager device + GPU identity + per-client channel setup state. */
extern bool manager;
extern u8 manager_gpu_uuid[16];
extern bool manager_gpu_uuid_valid;
extern unsigned long client_channels_setup;
extern unsigned long client_channels_pending;
extern bool rpc_loopback;

/* compute channel carve (region geometry helper, defined in main). */
int sev_gpu_compute_carve(struct sev_gpu_data_dev *dd, u32 idx,
			  u64 *userd_gpa, u64 *gpfifo_gpa,
			  u64 *pushbuf_gpa, u64 *userd_off,
			  u64 *gpfifo_off, u64 *pushbuf_off,
			  u64 *enc_gpa, u64 *enc_off);
/* bring-up watcher (defined in main). */
void sev_gpu_bringup_arm(u32 vm, u32 h_client, u32 h_channel);
void sev_gpu_bringup_disarm(u32 vm);
bool sev_gpu_bringup_poll(void);
void bringup_all(void);

/* Manager-to-client native OS-event completion relay (defined in main). */
int sev_gpu_event_drain(struct sev_gpu_dev *d);



/* KMB call result codes (NV-style) shared by kmb + handshake + rpc. */
#define SEV_KMB_NV_OK   0u
#define SEV_KMB_NV_ERR  RPC_FWD_ERR

/* auto-mTLS module params (defined in sev_gpu_main.c). */
extern bool auto_mtls;
extern uint auto_mtls_wait_ms;

/* Cross-cluster helpers (defined in main/handshake; declared for kmb etc.). */
bool sev_gpu_wait_comm_key(u32 vm, unsigned int timeout_ms);
void sev_gpu_hs_client_maybe_run(u8 vm);
void sev_gpu_manager_note_client_active(u32 vm_id);
int sev_gpu_get_psk(u8 out[SEV_GPU_COMM_KEY_LEN]);
int sev_gpu_assign_channel(u8 vm_id, u32 keyspace,
			   u32 in_h_client, u32 in_h_channel,
			   u32 in_channel_id, u32 *out_channel_id,
			   u32 *out_h_client, u32 *out_h_channel);
void ivshmem_ring(struct sev_gpu_dev *d, u16 peer, u16 vector);
u16  sev_gpu_manager_peer(struct sev_gpu_dev *d);

#endif /* SEV_GPU_STATE_H */
