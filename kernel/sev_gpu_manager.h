/*
 * sev_gpu_manager.h
 * 
 * SEV GPU Manager - Kernel Module Header
 * Manages GPU sharing between multiple VMs via shared memory
 */

#ifndef SEV_GPU_MANAGER_H
#define SEV_GPU_MANAGER_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Magic for ioctl commands */
#define SEV_GPU_IOC_MAGIC 'G'

/* Maximum number of client VMs supported */
#define SEV_GPU_MAX_VMS 8
#define SEV_GPU_MAX_CHANNELS_PER_VM 32
#define SEV_GPU_CC_POOL_MAX 32
#define RPC_FWD_ERR 0x0000ffffu  /* transport failure (NV_ERR_GENERIC-like) */
#define SHMEM_MAGIC 0xDEADBEEFCAFEBABEULL
#define SEV_GPU_BRINGUP_MAX_CAND 32u
#define SEV_GPU_CHAN_KIND_CE 0
#define SEV_GPU_CHAN_KIND_COMPUTE 1
#define RPC_IDLE_POLL_MS 50
#define SEV_GPU_BRINGUP_POLL_MS 2u

/*
 * Shared memory layout (within the ivshmem BAR2 region).
 *
 * The control-plane regions (header/request/grant) are small and fixed.
 * The data region occupies the remainder of the BAR and is split evenly
 * across SEV_GPU_MAX_VMS; its per-VM size is computed at runtime by the
 * manager and published via the header (vm_data_offset/vm_data_size).
 */
#define SEV_GPU_HEADER_SIZE         (64 * 1024)         /* 64 KB header  */
#define SEV_GPU_REQUEST_REGION_SIZE (1 * 1024 * 1024)   /* 1 MB control  */
#define SEV_GPU_GRANT_REGION_SIZE   (1 * 1024 * 1024)   /* 1 MB control  */
#define SEV_GPU_CONTROL_SIZE \
    (SEV_GPU_HEADER_SIZE + SEV_GPU_REQUEST_REGION_SIZE + SEV_GPU_GRANT_REGION_SIZE)

/*
 * ivshmem PCI device (QEMU ivshmem-doorbell).
 *   BAR0 = device registers (MMIO)
 *   BAR1 = MSI-X table
 *   BAR2 = shared memory region (cross-VM)
 */
/*
 * Transport (ivshmem) definitions moved to sev_gpu_transport.h — the swap
 * boundary. IVSHMEM_*, SEV_GPU_MANAGER_PEER_ID, and the transport interface
 * now live there. See sev_gpu_transport.h.
 */
#include "sev_gpu_transport.h"

/* Message types */
#define GPU_REQ_TIME        0x01
#define GPU_REQ_RELEASE     0x02
#define GPU_REQ_STATUS      0x03
#define GPU_GRANT           0x10
#define GPU_GRANT_DENIED    0x11
#define GPU_CTX_SWITCH      0x20

/* Status codes */
#define GPU_STATUS_IDLE     0x00
#define GPU_STATUS_REQUESTING 0x01
#define GPU_STATUS_GRANTED  0x02
#define GPU_STATUS_RUNNING  0x03
#define GPU_STATUS_DONE     0x04
#define GPU_STATUS_ERROR    0xFF

/* GPU Request from VM */
typedef struct {
    uint8_t vm_id;              /* Source VM (0-7) */
    uint8_t priority;           /* Priority level (0-255) */
    uint16_t msg_type;          /* GPU_REQ_TIME, etc. */
    uint32_t duration_us;       /* Requested GPU time in microseconds */
    uint32_t required_vram_mb;  /* Required VRAM */
    uint64_t timestamp_ns;      /* Request timestamp */
    uint8_t reserved[32];
    uint8_t data[128];          /* Opaque GPU command data */
} __attribute__((packed)) gpu_request_t;

/* GPU Grant from Kernel */
typedef struct {
    uint8_t vm_id;              /* Target VM */
    uint8_t status;             /* GPU_STATUS_GRANTED, etc. */
    uint32_t allocated_us;      /* Actual allocated time */
    uint64_t grant_start_ns;    /* When GPU available */
    uint64_t grant_end_ns;      /* When to release */
    uint8_t reserved[32];
} __attribute__((packed)) gpu_grant_t;

/* Control state broadcast by kernel */
typedef struct {
    uint32_t version;
    uint32_t current_owner;     /* Which VM owns GPU now */
    uint64_t switch_time_ns;    /* When next switch occurs */
    uint8_t active_vms;         /* Bitmask of VMs with requests */
    uint8_t reserved[55];
} __attribute__((packed)) control_state_t;

/* Shared memory header (first page) */
typedef struct {
    uint64_t magic;             /* 0xDEADBEEFCAFEBABE */
    uint32_t version;           /* Protocol version */
    uint32_t num_vms;           /* Number of registered VMs */
    
    /* Memory region offsets from base */
    uint64_t request_region_off;
    uint64_t grant_region_off;
    uint64_t data_region_off;
    
    /* Sizes */
    uint32_t request_region_size;
    uint32_t grant_region_size;
    uint32_t data_region_size;
    
    /* Per-VM data buffer offsets */
    uint64_t vm_data_offset[SEV_GPU_MAX_VMS];
    uint32_t vm_data_size[SEV_GPU_MAX_VMS];
    
    /* Synchronization primitives (file descriptors exported to user) */
    int request_notify_fd;      /* Signals new request available */
    int grant_notify_fd;        /* Signals new grant available */

    /*
     * ivshmem peer id of the manager VM, published so clients ring the
     * correct doorbell regardless of VM launch order (the manager is no
     * longer assumed to be IVPosition 0).
     */
    uint32_t manager_peer_id;

    uint8_t reserved[252];
} __attribute__((packed)) sev_gpu_shmem_header_t;

/*
 * Per-VM PRIVATE data region (hardware-isolated).
 *
 * Each client VM's GPU-work staging buffer lives in its OWN ivshmem region
 * (a separate ivshmem-plain device backed by a dedicated host file), NOT in
 * the shared control BAR. QEMU only maps a given data region into that one
 * client VM and the manager VM, so a client physically cannot reach another
 * client's bytes -- the isolation is enforced by the hypervisor, not by the
 * guest kernel.
 *
 * The region begins with this header (one page); the AES-GCM staging payload
 * follows at SEV_GPU_DATA_HEADER_SIZE. Manager and client coordinate over the
 * private region via the state word (NAPI-style: a single doorbell "kick" on
 * the shared control plane, then poll this header until the state advances).
 */
#define SEV_GPU_DATA_MAGIC        0xDA7A5EF6CAFEF00DULL
#define SEV_GPU_DATA_VERSION      1
#define SEV_GPU_DATA_HEADER_SIZE  4096                    /* payload starts here */
#define SEV_GPU_DATA_REGION_DEFAULT (64UL * 1024 * 1024)  /* default region size */
#define SEV_GPU_DATA_OWNER_NONE   0xFFFFFFFFU             /* unbound / free slot  */

/* data-region state word (owner/manager handshake for one job) */
#define SEV_GPU_DATA_FREE     0x00   /* not bound to any VM            */
#define SEV_GPU_DATA_BOUND    0x01   /* bound to owner_vm_id, idle     */
#define SEV_GPU_DATA_STAGED   0x02   /* client wrote work, kick sent   */
#define SEV_GPU_DATA_INFLIGHT 0x03   /* manager/GPU copying            */
#define SEV_GPU_DATA_DONE     0x04   /* result ready for client        */

typedef struct {
    uint64_t magic;             /* SEV_GPU_DATA_MAGIC                       */
    uint32_t version;           /* SEV_GPU_DATA_VERSION                     */
    uint32_t pool_index;        /* manager pool slot (probe order)          */
    uint32_t owner_vm_id;       /* current owner; SEV_GPU_DATA_OWNER_NONE   */
    uint32_t state;             /* SEV_GPU_DATA_* for the in-flight job      */
    uint64_t region_size;       /* total region bytes (header + payload)    */
    uint64_t payload_off;       /* == SEV_GPU_DATA_HEADER_SIZE              */
    uint64_t payload_size;      /* region_size - SEV_GPU_DATA_HEADER_SIZE   */

    /* AES-GCM framing for the current job (payload holds the ciphertext). */
    uint64_t data_len;          /* ciphertext length within payload         */
    uint8_t  iv[16];            /* GCM nonce/IV                             */
    uint8_t  auth_tag[16];      /* GCM authentication tag                   */

    uint64_t seq;               /* job sequence number (debug/ordering)     */

    /*
     * Mediated CE secure-copy request (D4.3 / Stage 2, Option A). A client
     * fills these payload-relative fields, flips @state to SEV_GPU_DATA_STAGED
     * and kicks the manager; the manager verifies the client owns @req_channel_id
     * (its OWN pool slot, never the client-written owner_vm_id), drives the GPU
     * Copy Engine, writes @req_status and flips @state to SEV_GPU_DATA_DONE. The
     * offsets mirror sev_gpu_ioctl_submit_t and are validated by the manager.
     */
    uint32_t req_channel_id;    /* in:  channel to submit on (client owns)  */
    uint32_t req_flags;         /* in:  SEV_GPU_SUBMIT_F_* bits             */
    uint32_t req_generation;    /* in:  KMB epoch to pin to (0 = unpinned)  */
    int32_t  req_status;        /* out: 0 = success; <0 = -errno           */
    uint64_t req_src_offset;    /* in:  H2D: payload off; D2H: VRAM off     */
    uint64_t req_dst_offset;    /* in:  H2D: VRAM off; D2H: payload off     */
    uint64_t req_length;        /* in:  transfer length in bytes           */
    uint64_t req_auth_tag_offset; /* in: GCM auth-tag payload off (16-align) */
    uint64_t req_iv_offset;     /* in:  GCM IV payload off (16-align)       */

    /*
     * Compute work-submission request (todo#7 / Stage 2). When @req_kind ==
     * SEV_GPU_REQ_KIND_SUBMIT_WORK the manager ignores the CE-copy operands
     * above and instead rings the doorbell of the client's GPFIFO channel: the
     * client has already published GP_PUT into its shared USERD, so the manager
     * (the sole doorbell-ringer) only needs to nudge the GPU host to re-read it.
     * @req_h_client / @req_h_channel are the client's RM handles for the
     * channel, valid in the manager's per-client replay namespace.
     */
    uint32_t req_kind;          /* in:  SEV_GPU_REQ_KIND_* (0 = CE copy)    */
    uint32_t req_h_client;      /* in:  work-submit: client RM client handle */
    uint32_t req_h_channel;     /* in:  work-submit: client RM channel handle*/

    /*
     * CLIENT's BAR2 GPA for its own data ivshmem device. Written by the client
     * on its first RPC (at which point the manager has already initialised the
     * header); read by the manager's sev_gpu_shadow_db_impl so MAP_MEMORY
     * returns a pLinearAddress in the CLIENT's address space, not the manager's,
     * so the client's mmap redirect can find it.
     */
    uint64_t client_mem_phys;

    /*
     * Independent compute-doorbell path (deliberately separate from the
     * req_kind/SUBMIT_WORK machinery, whose correctness is untested). This is a
     * pure "mirror the doorbell ring" pipe: the client's QEMU overlay captures
     * the token CUDA writes to the usermode window (+0x90) and publishes it here
     * (@db_token) while bumping @db_seq. The manager notices @db_seq advanced,
     * reads @db_token, and rings the REAL GPU usermode doorbell with exactly
     * that token via sev_gpu_rm_ring_doorbell(). No channel lookup, no CC gate,
     * no interaction with @state / @req_kind. @db_seq is the reason/edge marker:
     * the manager tracks the last value it serviced, so a bump == "a compute
     * doorbell was rung," independent of the per-instance irq_status bit.
     */
    uint32_t db_token;          /* in:  raw NV_VIRTUAL_FUNCTION_DOORBELL token */
    uint32_t db_seq;            /* in:  monotonically bumped on each ring       */

    uint8_t  reserved[76];
} __attribute__((packed)) sev_gpu_data_header_t;

/* req_kind discriminator for the data-region request (CE copy vs work submit) */
#define SEV_GPU_REQ_KIND_CE_COPY     0x00u  /* mediated CE secure-copy        */
#define SEV_GPU_REQ_KIND_SUBMIT_WORK 0x01u  /* ring a GPFIFO channel doorbell */
#define SEV_GPU_REQ_KIND_FLUSH_ALL   0x02u  /* ring all assigned compute channel doorbells */
#define SEV_GPU_REQ_KIND_WLC_LAUNCH  0x03u  /* GPU-autonomous WLC decrypt-and-launch (Fork B C+D) */

/* ioctl commands */

/* Register a VM with the manager */
typedef struct {
    uint8_t vm_id;
    char vm_name[32];
    pid_t vm_pid;
} sev_gpu_ioctl_register_vm_t;

#define SEV_GPU_IOC_REGISTER_VM \
    _IOW(SEV_GPU_IOC_MAGIC, 1, sev_gpu_ioctl_register_vm_t)

/* Get shared memory physical address and size */
typedef struct {
    uint64_t phys_addr;
    uint64_t size;
} sev_gpu_ioctl_get_shmem_t;

#define SEV_GPU_IOC_GET_SHMEM \
    _IOR(SEV_GPU_IOC_MAGIC, 2, sev_gpu_ioctl_get_shmem_t)

/* Submit GPU request */
typedef struct {
    uint8_t vm_id;
    uint32_t duration_us;
    uint8_t priority;
} sev_gpu_ioctl_request_gpu_t;

#define SEV_GPU_IOC_REQUEST_GPU \
    _IOW(SEV_GPU_IOC_MAGIC, 3, sev_gpu_ioctl_request_gpu_t)

/* Get GPU grant */
typedef struct {
    uint8_t vm_id;
    gpu_grant_t grant;
} sev_gpu_ioctl_get_grant_t;

#define SEV_GPU_IOC_GET_GRANT \
    _IOR(SEV_GPU_IOC_MAGIC, 4, sev_gpu_ioctl_get_grant_t)

/* Release GPU */
typedef struct {
    uint8_t vm_id;
} sev_gpu_ioctl_release_gpu_t;

#define SEV_GPU_IOC_RELEASE_GPU \
    _IOW(SEV_GPU_IOC_MAGIC, 5, sev_gpu_ioctl_release_gpu_t)

/* Query this peer's role and ivshmem identity */
typedef struct {
    uint8_t is_manager;     /* 1 if this node is the GPU manager */
    int32_t ivposition;     /* ivshmem peer id (-1 if unavailable) */
    uint8_t num_vms;        /* registered client VMs */
} sev_gpu_ioctl_get_role_t;

#define SEV_GPU_IOC_GET_ROLE \
    _IOR(SEV_GPU_IOC_MAGIC, 6, sev_gpu_ioctl_get_role_t)

/* Block until a GPU grant arrives (interrupt-driven on clients) */
typedef struct {
    uint8_t vm_id;
    int32_t timeout_ms;     /* <0 = wait forever */
    gpu_grant_t grant;      /* filled in on success */
} sev_gpu_ioctl_wait_grant_t;

#define SEV_GPU_IOC_WAIT_GRANT \
    _IOWR(SEV_GPU_IOC_MAGIC, 7, sev_gpu_ioctl_wait_grant_t)

/*
 * Per-VM PRIVATE data device ioctls (on /dev/sev_gpu_dataN).
 *
 * Each private data region is a separate ivshmem-plain device. Userspace mmaps
 * the WHOLE device (offset 0): the first page is the sev_gpu_data_header_t, the
 * payload follows at payload_off. No offset bounding is needed -- the region is
 * already hardware-isolated to this VM (+ the manager) by QEMU.
 */

/* Query a private data region's geometry + current binding/state. */
typedef struct {
    uint32_t pool_index;    /* out: device/pool index on this VM            */
    uint32_t owner_vm_id;   /* out: current owner (SEV_GPU_DATA_OWNER_NONE) */
    uint32_t state;         /* out: SEV_GPU_DATA_*                          */
    uint32_t is_manager;    /* out: 1 if this node is the manager           */
    uint64_t region_size;   /* out: total region bytes (header + payload)   */
    uint64_t payload_off;   /* out: == SEV_GPU_DATA_HEADER_SIZE             */
    uint64_t payload_size;  /* out: payload bytes                           */
} sev_gpu_ioctl_data_info_t;

#define SEV_GPU_IOC_DATA_INFO \
    _IOR(SEV_GPU_IOC_MAGIC, 8, sev_gpu_ioctl_data_info_t)

/*
 * Manager-only: (re)bind this private data region to an owner VM. The payload
 * is SCRUBBED (zeroed) before the binding changes, so a region reused by a new
 * owner can never expose the previous owner's bytes.
 */
typedef struct {
    uint32_t owner_vm_id;   /* in: new owner, or SEV_GPU_DATA_OWNER_NONE to free */
} sev_gpu_ioctl_data_bind_t;

#define SEV_GPU_IOC_DATA_BIND \
    _IOW(SEV_GPU_IOC_MAGIC, 9, sev_gpu_ioctl_data_bind_t)

/*
 * RM-RPC transport self-test (Phase D1).
 *
 * Issued on a CLIENT's control device (/dev/sev_gpu_manager, manager=0). The
 * opaque blob in data[0..size) is marshalled through this client's per-VM
 * ivshmem mailbox (see sev_gpu_rpc.h) to the manager and the reply is copied
 * back. With the manager in loopback mode (manager=1 rpc_loopback=1) the blob
 * round-trips unchanged, exercising the full mailbox/doorbell/replay path with
 * no GPU and no nvidia.ko loaded.
 */
#define SEV_GPU_RPC_TEST_MAX 4096

typedef struct {
    uint32_t cmd;       /* in:  opaque command tag (echoed back)         */
    uint32_t size;      /* in:  valid bytes in data[]; clamped to MAX     */
    int32_t  rm_status; /* out: status reported by the manager            */
    uint32_t reserved;
    uint8_t  data[SEV_GPU_RPC_TEST_MAX];  /* in/out: payload              */
} sev_gpu_ioctl_rpc_test_t;

#define SEV_GPU_IOC_RPC_TEST \
    _IOWR(SEV_GPU_IOC_MAGIC, 10, sev_gpu_ioctl_rpc_test_t)

/* =====================================================================
 * Phase D2..D4: confidential-compute GPU channel key delivery.
 *
 * The manager owns the GPU and EVERY GPU channel. It provisions a pool of
 * CC channels, assigns them to clients, fetches each channel's KMB (GPU key
 * material), seals it under a per-client comm key, and ships the ciphertext
 * to the client over the shared BAR. Clients never allocate channels and the
 * plaintext KMB/keys never appear in userspace or in host-visible memory.
 * ===================================================================== */

/*
 * Secure key-delivery tunnel region (Phase D2). A per-VM pair of shared-memory
 * rings carries a mutual-TLS session between the manager (TLS server) and each
 * client VM (TLS client); userspace (the keybroker agents) lay out the rings,
 * the kernel only mmaps the BAR. Lives in the gap above the control region.
 */
#define SEV_GPU_TLS_REGION_OFF   SEV_GPU_CONTROL_SIZE              /* 0x210000        */
#define SEV_GPU_TLS_SLOT_STRIDE  (64UL * 1024UL)                  /* per-VM tunnel slice */
#define SEV_GPU_TLS_RING_CAP     (16UL * 1024UL)                  /* per-ring data, pow2 */
#define SEV_GPU_TLS_C2M_OFF      (0UL)                            /* client->manager ring */
#define SEV_GPU_TLS_M2C_OFF      (SEV_GPU_TLS_SLOT_STRIDE / 2UL)  /* manager->client ring */
#define SEV_GPU_TLS_REGION_SIZE  ((unsigned long)SEV_GPU_MAX_VMS * SEV_GPU_TLS_SLOT_STRIDE)
/* Bytes userspace must mmap (from BAR offset 0) to reach the whole tunnel. */
#define SEV_GPU_TLS_MAP_SIZE     (SEV_GPU_TLS_REGION_OFF + SEV_GPU_TLS_REGION_SIZE)

/*
 * Deliver the manager<->client communication key into the driver. Userspace
 * (the keybroker) runs the mutual-TLS handshake over the tunnel rings, derives
 * a shared session key (TLS exporter), hands it down here and forgets it. The
 * driver uses it to seal the in-kernel KMB exchange; the KMB itself is produced,
 * transported and installed entirely in kernel space.
 */
#define SEV_GPU_COMM_KEY_LEN 32     /* AES-256 manager<->client comm key */

typedef struct {
    uint8_t vm_id;
    uint8_t key_len;                /* must equal SEV_GPU_COMM_KEY_LEN */
    uint8_t reserved[2];
    uint8_t key[SEV_GPU_COMM_KEY_LEN];
} sev_gpu_ioctl_set_comm_key_t;

#define SEV_GPU_IOC_SET_COMM_KEY \
    _IOW(SEV_GPU_IOC_MAGIC, 11, sev_gpu_ioctl_set_comm_key_t)

/*
 * GPU CC_KMB layout, mirrored byte-for-byte from the NVIDIA driver's cc_drv.h
 * so the kernel can carry/seal it opaquely. Sizes are in dwords.
 */
#define SEV_CC_AES_256_GCM_IV_SIZE_DWORD   3
#define SEV_CC_AES_256_GCM_KEY_SIZE_DWORD  8
#define SEV_CC_HMAC_NONCE_SIZE_DWORD       8
#define SEV_CC_HMAC_KEY_SIZE_DWORD         8

struct sev_cc_aes_cryptobundle {
    __u32 iv[SEV_CC_AES_256_GCM_IV_SIZE_DWORD];
    __u32 key[SEV_CC_AES_256_GCM_KEY_SIZE_DWORD];
    __u32 ivMask[SEV_CC_AES_256_GCM_IV_SIZE_DWORD];
};

struct sev_cc_hmac_cryptobundle {
    __u32 nonce[SEV_CC_HMAC_NONCE_SIZE_DWORD];
    __u32 key[SEV_CC_HMAC_KEY_SIZE_DWORD];
};

struct sev_cc_kmb {
    struct sev_cc_aes_cryptobundle encryptBundle;
    union {
        struct sev_cc_hmac_cryptobundle hmacBundle;
        struct sev_cc_aes_cryptobundle  decryptBundle;
    } u;
    __u32 bIsWorkLaunch;
};

/*
 * Sealed KMB mailbox, one fixed-stride slot per client VM, living in the gap
 * between the TLS tunnel region and the RM-RPC mailbox region (0x290000..).
 */
#define SEV_GPU_KMB_REGION_OFF \
    (SEV_GPU_TLS_REGION_OFF + SEV_GPU_TLS_REGION_SIZE)        /* 0x290000 */
#define SEV_GPU_KMB_SLOT_STRIDE  (4UL * 1024UL)              /* one page per VM */
#define SEV_GPU_KMB_REGION_SIZE \
    ((unsigned long)SEV_GPU_MAX_VMS * SEV_GPU_KMB_SLOT_STRIDE)
#define SEV_GPU_KMB_MAGIC        0x314d4b53u  /* "SKM1" */

/* Mailbox handshake states (written by manager/client, observed by the other). */
#define SEV_GPU_KMB_IDLE   0u  /* empty / consumed                       */
#define SEV_GPU_KMB_READY  1u  /* manager posted a sealed KMB            */
#define SEV_GPU_KMB_ACK    2u  /* client consumed and installed it       */

#define SEV_GPU_KMB_NONCE_LEN  12   /* AES-GCM 96-bit nonce */
#define SEV_GPU_KMB_TAG_LEN    16   /* AES-GCM 128-bit tag  */
#define SEV_GPU_KMB_CT_MAX     256  /* >= sizeof(struct sev_cc_kmb) */

struct sev_gpu_kmb_mbox {
    volatile __u32 magic;       /* SEV_GPU_KMB_MAGIC once initialised   */
    volatile __u32 state;       /* SEV_GPU_KMB_{IDLE,READY,ACK}         */
    __u32 vm_id;                /* target client (manager-assigned)     */
    __u32 channel_id;           /* GPU channel this KMB belongs to      */
    __u32 keyspace;             /* channel keyspace selector            */
    __u32 seq;                  /* monotonically increasing per slot    */
    __u32 ct_len;               /* sealed plaintext length (= KMB size) */
    __u32 reserved;
    __u8  nonce[SEV_GPU_KMB_NONCE_LEN];
    __u8  tag[SEV_GPU_KMB_TAG_LEN];
    __u8  ct[SEV_GPU_KMB_CT_MAX];   /* AES-256-GCM ciphertext of the KMB */
};

/*
 * GPU identity descriptor region (control-plane enumeration bootstrap).
 *
 * Unmodified CUDA on a GPU-less client must enumerate a GPU before any RM
 * escape can be forwarded. The manager owns the real GPU, so it exports that
 * GPU's stable identity (via nvidia.ko's sev_gpu_export_local_gpu) and publishes
 * it here, in the gap above the KMB region. The client reads it and materializes
 * a synthetic /dev/nvidiaN with the same gpu_id (via sev_gpu_client_register_gpu),
 * so the forwarded enumeration controls on the real GPU line up. One page, plain
 * data, no handshake. The struct mirrors sev_gpu_card_desc_t in nvidia.ko's
 * nv.c byte-for-byte; cross-module calls pass sizeof() to reject any mismatch.
 */
#define SEV_GPU_GPUDESC_REGION_OFF \
    (SEV_GPU_KMB_REGION_OFF + SEV_GPU_KMB_REGION_SIZE)        /* 0x298000 */
#define SEV_GPU_GPUDESC_REGION_SIZE  (4UL * 1024UL)          /* one page */
#define SEV_GPU_CARD_DESC_VERSION    2u

/*
 * =====================================================================
 * DB REPLAY RING (experimental full-trap-and-replay of the doorbell MMIO page)
 * ---------------------------------------------------------------------
 * The client traps EVERY read and write on the whole doorbell page and posts it
 * here as a request; the manager performs the REAL MMIO access against its
 * mapped BAR1 VF doorbell page and posts the result back. For reads the client
 * spin-waits on @state until the manager fills @value. This is a synchronous
 * request/response mailbox (one in-flight op per VM is sufficient: a single vCPU
 * MMIO access blocks until it returns). Separate from the db_token/db_seq path,
 * which stays in the code but is not hooked when this ring is active.
 *
 * Lives at a fixed per-VM offset in the client's data region (BAR2), one slot
 * per VM. Both QEMU (client + manager) and the manager module map the same file.
 * =====================================================================
 */
#define SEV_GPU_DBRING_REGION_OFF \
    (SEV_GPU_GPUDESC_REGION_OFF + SEV_GPU_GPUDESC_REGION_SIZE) /* 0x299000 */
#define SEV_GPU_DBRING_SLOT_STRIDE   (4UL * 1024UL)            /* one page per VM */
#define SEV_GPU_DBRING_REGION_SIZE \
    ((unsigned long)SEV_GPU_MAX_VMS * SEV_GPU_DBRING_SLOT_STRIDE)
#define SEV_GPU_DBRING_MAGIC         0x31524253u  /* "SBR1" */

/* slot->state values */
#define SEV_GPU_DBRING_IDLE     0u   /* empty, ready for a new op            */
#define SEV_GPU_DBRING_REQ      1u   /* client posted a request              */
#define SEV_GPU_DBRING_DONE     2u   /* manager posted the result            */

/* slot->op values */
#define SEV_GPU_DBRING_OP_READ  0u
#define SEV_GPU_DBRING_OP_WRITE 1u
#define SEV_GPU_DBRING_OP_DOORBELL 0x80000000u /* high bit: real +0x90 doorbell */

typedef struct {
    uint32_t magic;      /* SEV_GPU_DBRING_MAGIC                              */
    volatile uint32_t state; /* SEV_GPU_DBRING_* (client sets REQ, mgr DONE)  */
    uint32_t seq;        /* bumped by client per op (edge/debug)             */
    uint32_t op;         /* SEV_GPU_DBRING_OP_READ / _WRITE                  */
    uint64_t offset;     /* byte offset within the doorbell page             */
    uint32_t size;       /* access size (4)                                  */
    uint32_t value;      /* write: client->mgr value; read: mgr->client value*/
    uint32_t rd_result;  /* read: the real MMIO value the manager read back  */
    uint32_t status;     /* 0 = applied ok                                   */
    uint8_t  reserved[4064];
} __attribute__((packed)) sev_gpu_dbring_slot_t;

static inline u64 sev_gpu_dbring_slot_off(u32 vm)
{
    return SEV_GPU_DBRING_REGION_OFF + (u64)vm * SEV_GPU_DBRING_SLOT_STRIDE;
}

/*
 * Manager-to-client OS-event completions. The manager's real RM notifier is
 * intentionally pointer-free; on completion it publishes this scalar record
 * and rings the client. The client resolves event_fd against its local
 * NV_ESC_ALLOC_OS_EVENT state and performs the native nv_post_event wakeup.
 */
#define SEV_GPU_EVENT_RING_OFF \
    (SEV_GPU_DBRING_REGION_OFF + SEV_GPU_DBRING_REGION_SIZE) /* 0x2a1000 */
#define SEV_GPU_EVENT_RING_SIZE       (4UL * 1024UL)
#define SEV_GPU_EVENT_RING_MAGIC      0x31524553u /* "SER1" */
#define SEV_GPU_EVENT_RING_VERSION    1u
#define SEV_GPU_EVENT_RING_CAPACITY   128u
#define SEV_GPU_EVENT_DATA_VALID      (1u << 16)

typedef struct {
    uint32_t h_event_client;
    uint32_t h_event;
    uint32_t event_fd;
    uint32_t notify_index;
    uint32_t info32;
    uint32_t info16_flags; /* info16 in bits 15:0; DATA_VALID in bit 16 */
} __attribute__((packed)) sev_gpu_event_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t prod;
    uint32_t cons;
    uint32_t dropped;
    uint32_t reserved_header[3];
    sev_gpu_event_entry_t entries[SEV_GPU_EVENT_RING_CAPACITY];
    uint8_t reserved[SEV_GPU_EVENT_RING_SIZE - 32 -
                     SEV_GPU_EVENT_RING_CAPACITY *
                     sizeof(sev_gpu_event_entry_t)];
} __attribute__((packed)) sev_gpu_event_ring_t;

typedef struct {
    __u32 version;          /* SEV_GPU_CARD_DESC_VERSION                 */
    __u32 valid;            /* 1 once the manager has filled it          */
    __u32 domain;           /* PCI domain                                */
    __u32 gpu_id;           /* RM gpu_id (must equal the real GPU's)     */
    __u32 interrupt_line;   /* legacy IRQ line                           */
    __u16 vendor_id;
    __u16 device_id;
    __u8  bus;
    __u8  slot;
    __u8  function;
    __u8  pad0;
    __u32 pad1;             /* explicit pad so u64s are 8-byte aligned   */
    __u64 reg_address;      /* BAR0 (regs) CPU physical address          */
    __u64 reg_size;
    __u64 fb_address;       /* BAR1 (fb) CPU physical address            */
    __u64 fb_size;
    __u8  uuid[16];         /* real GPU UUID (GPU_UUID_LEN); client sets  *
                             * it as the synthetic device's cached UUID   */
} sev_gpu_card_desc_t;

/*
 * Manager assigns a GPU channel to a client (manager-authoritative policy).
 * Clients cannot request channels; the manager picks a free pre-provisioned
 * channel of the requested keyspace from its own pool (see PROVISION_POOL) and
 * fills channel_id/h_client/h_channel on return. If the caller passes non-zero
 * h_client+h_channel those exact handles are used. With no pool entry and no
 * handles, a placeholder KMB is staged for the caller-supplied channel_id.
 */
#define SEV_GPU_CC_ALLOC_FORCE_CE_ID  0x1u  /* keyspace names a specific CE/LCE */

typedef struct {
    __u8  vm_id;            /* in:  client the channel is assigned to */
    __u8  reserved[3];
    __u32 channel_id;       /* in/out: pool mode => chosen id (out);
                             *         placeholder mode => caller id (in) */
    __u32 keyspace;         /* in:  channel keyspace selector         */
    __u32 h_client;         /* in/out: RM client handle (out in pool mode;
                             *         0 in => placeholder / pool pick)  */
    __u32 h_channel;        /* in/out: RM channel handle (see h_client)  */
} sev_gpu_ioctl_assign_t;

#define SEV_GPU_IOC_ASSIGN_CHANNEL \
    _IOWR(SEV_GPU_IOC_MAGIC, 12, sev_gpu_ioctl_assign_t)

/*
 * Manager seals the assigned channel's KMB under the client comm key and posts
 * it to that client's mailbox, then waits for the client to ack.
 */
typedef struct {
    __u8  vm_id;            /* in:  target client                     */
    __u8  reserved[3];
    __u32 channel_id;       /* in:  which assigned channel to send    */
    __u32 timeout_ms;       /* in:  0 => default; wait for client ack */
    __u8  fp[8];            /* out: SHA-256[:8] of the plaintext KMB  */
} sev_gpu_ioctl_send_kmb_t;

#define SEV_GPU_IOC_SEND_KMB \
    _IOWR(SEV_GPU_IOC_MAGIC, 13, sev_gpu_ioctl_send_kmb_t)

/*
 * Client waits for a sealed KMB, unseals it with its comm key, and installs it
 * into the matching channel. Returns the channel binding and a fingerprint of
 * the plaintext KMB so a self-test can confirm both ends agree.
 */
typedef struct {
    __u32 timeout_ms;       /* in:  0 => default                      */
    __u32 channel_id;       /* out: channel this KMB belongs to       */
    __u32 keyspace;         /* out: channel keyspace selector         */
    __u32 reserved;
    __u8  fp[8];            /* out: SHA-256[:8] of the plaintext KMB  */
} sev_gpu_ioctl_recv_kmb_t;

#define SEV_GPU_IOC_RECV_KMB \
    _IOWR(SEV_GPU_IOC_MAGIC, 14, sev_gpu_ioctl_recv_kmb_t)

/*
 * Manager pre-allocates a pool of confidential-compute channels on the real GPU
 * (each its own kernel RM client) so ASSIGN can hand them out without any client
 * ever allocating a channel. Requires nvidia.ko's CC-channel allocator to be
 * bound (manager VM); returns -ENODEV otherwise.
 */
typedef struct {
    __u32 keyspace;         /* in:  CE/LCE keyspace for the channels  */
    __u32 count;            /* in:  how many channels to provision    */
    __u32 flags;            /* in:  SEV_GPU_CC_ALLOC_* bits           */
    __u32 provisioned;      /* out: how many were actually allocated  */
} sev_gpu_ioctl_provision_t;

#define SEV_GPU_IOC_PROVISION_POOL \
    _IOWR(SEV_GPU_IOC_MAGIC, 15, sev_gpu_ioctl_provision_t)

/*
 * Stage 2 secure-copy bounce buffer. The GPU Copy Engine DMAs the client's
 * AES-256-GCM ciphertext straight out of the requesting VM's PRIVATE 64 MiB
 * data region (the per-VM ivshmem-plain device -- SEV-isolated: only that
 * client's QEMU and the manager's QEMU map it) and decrypts it into protected
 * VRAM, and vice-versa. The control BAR is metadata-only (4 MiB) and is NOT
 * used for payload.
 *
 * The submit offsets below are byte offsets into that data region's PAYLOAD
 * (i.e. relative to SEV_GPU_DATA_HEADER_SIZE); the GCM tag and IV live there too
 * and must be SEV_GPU_BOUNCE_ALIGN-aligned.
 */
#define SEV_GPU_BOUNCE_ALIGN  16UL   /* GCM tag/IV alignment the CE requires */

/*
 * Mediated work submission (D4.3 / Stage 2, Option A): the manager is the SOLE
 * entity that rings a channel's doorbell. A client asks the manager to launch a
 * CE secure-copy on a channel it owns; the manager verifies ownership, then
 * resolves these payload-relative offsets into physical addresses
 * (sysmem_base = data_devs[vm_id] phys + SEV_GPU_DATA_HEADER_SIZE) and drives
 * the GPU on its behalf. The fbmem (VRAM) operand is NOT a physical address: it
 * is a byte offset into the channel's RM-owned VRAM scratch buffer.
 *
 *   H2D (SEV_GPU_SUBMIT_F_ENCRYPT set): src_offset = data-region payload offset
 *       (sysmem, client-encrypted ciphertext); dst_offset = VRAM offset.
 *   D2H (clear):                         src_offset = VRAM offset;
 *       dst_offset = data-region payload offset (sysmem, CE writes ciphertext).
 *   auth_tag_offset / iv_offset are always data-region payload offsets (sysmem)
 *   and must be SEV_GPU_BOUNCE_ALIGN-aligned.
 */
#define SEV_GPU_SUBMIT_F_ENCRYPT  0x1u  /* set: host->device (encrypt); clear: d2h */

typedef struct {
    __u32 vm_id;            /* in:  requesting client (ownership checked) */
    __u32 channel_id;       /* in:  assigned channel to submit on         */
    __u32 flags;            /* in:  SEV_GPU_SUBMIT_F_* bits               */
    __u32 generation;       /* in:  KMB epoch to pin to (0 = unpinned)    */
    __u64 src_offset;       /* in:  H2D: payload off; D2H: VRAM off        */
    __u64 dst_offset;       /* in:  H2D: VRAM off; D2H: payload off        */
    __u64 length;           /* in:  transfer length in bytes              */
    __u64 auth_tag_offset;  /* in:  GCM auth-tag data-region payload off   */
    __u64 iv_offset;        /* in:  GCM IV data-region payload offset      */
} sev_gpu_ioctl_submit_t;

#define SEV_GPU_IOC_SUBMIT_COPY \
    _IOW(SEV_GPU_IOC_MAGIC, 16, sev_gpu_ioctl_submit_t)

/*
 * Client-driven mediated copy (D4.3 / Stage 2, Option A): a client cannot ring
 * a GPU doorbell, so it asks the manager to launch the CE secure-copy on its
 * behalf. The client stages the same parameters as SEV_GPU_IOC_SUBMIT_COPY into
 * its PRIVATE data-region header (state SEV_GPU_DATA_STAGED) and kicks the
 * manager; the manager verifies ownership, drives the GPU and returns the
 * result. The @vm_id field is ignored here -- the manager identifies the caller
 * from the physical data region, not from any client-written value. The ioctl
 * blocks until the manager completes the copy (or times out).
 */
#define SEV_GPU_IOC_REQUEST_COPY \
    _IOW(SEV_GPU_IOC_MAGIC, 19, sev_gpu_ioctl_submit_t)

/*
 * Client-side data-plane crypto (D4.3b): the client uses the channel KMB it
 * installed (RECV_KMB) to AES-256-GCM the bulk payload it stages in the shared
 * bounce region. The KMB never leaves the client kernel -- userspace only ever
 * sees ciphertext + IV + tag. The exposed iv[] is the pre-mask 96-bit message
 * counter; the GPU CE derives the real GCM IV as (counter XOR ivMask).
 */
#define SEV_GPU_CRYPT_F_DECRYPT             0x1u  /* op:  0=encrypt, 1=decrypt        */
#define SEV_GPU_CRYPT_F_USE_DECRYPT_BUNDLE  0x2u  /* key: 0=encryptBundle(h2d),        \
						   *      1=decryptBundle(d2h)        */
#define SEV_GPU_CRYPT_MAX  (1u << 16)             /* max bytes per crypt op           */

/*
 * IV-reuse guard (D4.3d): the 96-bit GCM IV is (message-counter XOR ivMask), so
 * a (key, counter) pair must never repeat. The client refuses to encrypt once a
 * bundle's counter reaches this threshold, forcing a key rotation.
 */
#define SEV_GPU_IV_ROTATE_THRESHOLD  (1ull << 32)

typedef struct {
    __u32 channel_id;       /* in:     installed channel                  */
    __u32 flags;            /* in:     SEV_GPU_CRYPT_F_* bits             */
    __u32 length;           /* in:     payload byte length                */
    __u32 generation;       /* out(enc): KMB epoch used;                   \
			     * in(dec):  expected epoch (0 = unchecked)   */
    __u64 data;             /* in/out: user buffer (length bytes)         */
    __u8  iv[12];           /* out(enc)/in(dec): 96-bit message counter   */
    __u8  tag[16];          /* out(enc)/in(dec): GCM auth tag             */
} sev_gpu_ioctl_crypt_t;

#define SEV_GPU_IOC_CRYPT \
    _IOWR(SEV_GPU_IOC_MAGIC, 17, sev_gpu_ioctl_crypt_t)

/*
 * Channel data-plane status (D4.3d, client): lets userspace observe the current
 * KMB epoch (generation) and per-direction message counters for an installed
 * channel, so it can detect a key rotation and proactively request a fresh KMB.
 */
typedef struct {
    __u32 channel_id;       /* in:  channel to query                      */
    __u32 valid;            /* out: 1 if a KMB is installed               */
    __u32 generation;       /* out: current KMB epoch                     */
    __u32 keyspace;         /* out: channel keyspace                      */
    __u64 ctr_h2d;          /* out: host->device message counter          */
    __u64 ctr_d2h;          /* out: device->host message counter          */
} sev_gpu_ioctl_chan_status_t;

#define SEV_GPU_IOC_CHAN_STATUS \
    _IOWR(SEV_GPU_IOC_MAGIC, 18, sev_gpu_ioctl_chan_status_t)

/*
 * Compute work submission (todo#7 / Stage 2). A GPU-less client cannot ring a
 * GPU doorbell, so after it publishes GP_PUT into its (shared) USERD it asks the
 * manager -- the sole doorbell-ringer -- to nudge the GPU host to re-read the
 * channel's GPFIFO. The handles identify the channel in the manager's per-client
 * replay namespace (the same namespace the client's forwarded RM_ALLOCs built).
 */
typedef struct {
    __u32 h_client;         /* in:  client RM client handle               */
    __u32 h_channel;        /* in:  client RM GPFIFO channel handle       */
    __u32 vm_id;            /* in:  VM whose assignment registry scopes the
                             *      doorbell (manager-local SUBMIT_WORK only;
                             *      the client REQUEST_SUBMIT path derives the
                             *      VM from its own data region and ignores it) */
} sev_gpu_ioctl_submit_work_t;

/*
 * Client-driven mediated work submission: the client stages @h_client /
 * @h_channel into its PRIVATE data-region header (req_kind = SUBMIT_WORK,
 * state SEV_GPU_DATA_STAGED) and kicks the manager, which rings the channel
 * doorbell and returns the result. Blocks until the manager completes.
 */
#define SEV_GPU_IOC_REQUEST_SUBMIT \
    _IOW(SEV_GPU_IOC_MAGIC, 20, sev_gpu_ioctl_submit_work_t)

/*
 * Manager-local work submission (debug / loopback): ring a channel doorbell
 * directly from the manager VM, bypassing the data-region handshake. Mirrors
 * SEV_GPU_IOC_SUBMIT_COPY for the compute path.
 */
#define SEV_GPU_IOC_SUBMIT_WORK \
    _IOW(SEV_GPU_IOC_MAGIC, 21, sev_gpu_ioctl_submit_work_t)

/*
 * Fork B GPU-autonomous compute launch (manager-local trigger). Instead of the
 * manager ringing the doorbell (Option A, SEV_GPU_IOC_SUBMIT_WORK), the manager
 * asks this client's per-client WLC (Work-Launch Channel) to decrypt-and-launch
 * the client's encrypted compute methods ENTIRELY on the GPU: the WLC decrypts
 * @push_size bytes of ciphertext (from the channel's enc region) into the
 * compute pushbuffer, writes the GPFIFO entry, advances GP_PUT, and rings the
 * VF doorbell. The manager CPU never sees plaintext. @chan_idx selects the
 * compute channel within (@vm_id)'s assignment registry.
 */
typedef struct {
    __u32 vm_id;            /* in:  VM whose compute channel to launch          */
    __u32 chan_idx;         /* in:  channel index in the VM assignment registry */
    __u32 push_size;        /* in:  bytes of client-encrypted compute methods   */
} sev_gpu_ioctl_wlc_launch_t;

#define SEV_GPU_IOC_WLC_LAUNCH \
    _IOW(SEV_GPU_IOC_MAGIC, 24, sev_gpu_ioctl_wlc_launch_t)

/*
 * Manager assigns a GR COMPUTE channel to a client (L3.3, Arch B "Option A":
 * allocate-on-assign). The manager carves three GPU-DMA-able pages out of the
 * assignee's PRIVATE data region (USERD + GPFIFO ring + method pushbuffer) and asks nvidia.ko to
 * build a compute channel backed by them, so the client can publish work in
 * place (zero-copy). Manager-only; clients never allocate channels. On return,
 * channel_id/h_client/h_channel name the manager-owned channel and
 * userd_off/gpfifo_off give the region-relative offsets the client maps.
 */
typedef struct {
    __u8  vm_id;            /* in:  client the channel is assigned to     */
    __u8  reserved[3];
    __u32 flags;            /* in:  NVOS04_FLAGS_* passthrough (0 = CC)   */
    __u32 channel_id;       /* out: id surfaced to the client             */
    __u32 h_client;         /* out: manager-owned RM client handle        */
    __u32 h_channel;        /* out: manager-owned RM channel handle       */
    __u32 reserved2;
    __u64 userd_off;        /* out: USERD offset in the VM's data region  */
    __u64 gpfifo_off;       /* out: GPFIFO ring offset in the data region */
    __u64 pushbuf_off;      /* out: method pushbuffer offset in DATA      */
    __u64 pushbuf_gpu_va;   /* out: GPU VA for NVA06F_GP_ENTRY address    */
} sev_gpu_ioctl_assign_compute_t;

#define SEV_GPU_IOC_ASSIGN_COMPUTE \
    _IOWR(SEV_GPU_IOC_MAGIC, 22, sev_gpu_ioctl_assign_compute_t)

/*
 * Client: ring the GPU doorbells for all compute channels the manager has
 * assigned to this VM. The client cannot ring doorbells itself; this ioctl
 * stages a FLUSH_ALL request in the PRIVATE data-region header, kicks the
 * manager, and blocks until the manager has rung every assigned compute
 * channel and returned a status. Call this after a kernel launch, before
 * cudaDeviceSynchronize(), to flush the work to the GPU.
 */
#define SEV_GPU_IOC_FLUSH_CHANNELS \
    _IO(SEV_GPU_IOC_MAGIC, 23)

/*
 * Fork B C+D: reserved metadata identifying the per-client WLC's KMB inside the
 * client keystore. The manager stages the WLC channel's KMB into its assignment
 * registry under SEV_GPU_WLC_CHANNEL_ID and delivers it over the existing sealed
 * KMB mailbox; the client installs it under the same id and uses it to
 * AES-256-GCM-encrypt the compute methods the WLC's CE decrypts on the GPU.
 */
#define SEV_GPU_WLC_CHANNEL_ID  0xFFFFFF01u  /* reserved: not a real hw channel id */
#define SEV_GPU_WLC_KEYSPACE    0xFFu         /* reserved keyspace tag (AAD metadata) */

/*
 * Fork B C: manager fetches the per-client WLC channels[0] KMB (via the
 * in-kernel GET_KMB path on the UVM-owned handle), seals it under the client
 * comm key and posts it to the client mailbox, then waits for the ack. The WLC
 * pool must already exist (created lazily on the first compute-channel assign).
 * Manager-only.
 */
typedef struct {
    __u8  vm_id;            /* in:  target client                     */
    __u8  reserved[3];
    __u32 timeout_ms;       /* in:  0 => default; wait for client ack */
    __u8  fp[8];            /* out: SHA-256[:8] of the plaintext KMB  */
} sev_gpu_ioctl_send_wlc_kmb_t;

#define SEV_GPU_IOC_SEND_WLC_KMB \
    _IOWR(SEV_GPU_IOC_MAGIC, 25, sev_gpu_ioctl_send_wlc_kmb_t)

/*
 * Fork B D: client-driven GPU-autonomous compute launch. The client has already
 * (1) installed its WLC KMB (RECV_KMB, channel SEV_GPU_WLC_CHANNEL_ID), (2)
 * AES-256-GCM-encrypted its compute methods with it, and (3) written the
 * ciphertext + auth tag into the compute channel's enc region. It then stages a
 * req_kind = SEV_GPU_REQ_KIND_WLC_LAUNCH request (req_channel_id = chan_idx,
 * req_length = push_size) into its PRIVATE data-region header and kicks the
 * manager, which asks the per-client WLC to decrypt-and-launch entirely on the
 * GPU. Reuses sev_gpu_ioctl_wlc_launch_t; @vm_id is ignored (the manager derives
 * the VM from the physical data region). Blocks until the manager completes.
 */
#define SEV_GPU_IOC_REQUEST_WLC_LAUNCH \
    _IOW(SEV_GPU_IOC_MAGIC, 26, sev_gpu_ioctl_wlc_launch_t)

/*
 * Fork B D: client-callable geometry query. The compute-channel reserve layout
 * (base + chan_idx * stride: USERD, GPFIFO, pushbuffer, enc cipher page, enc tag
 * page) is a pure function of the data region size and the channel index, and
 * the unified sev_gpu_manager.ko is built from identical source on both peers,
 * so the client can resolve the same region-relative offsets the manager carved
 * for a channel WITHOUT any cross-VM exchange. The client uses @enc_off to write
 * its AES-256-GCM ciphertext (at @enc_off) and auth tag (at @enc_off + 4096, one
 * page later) into its PRIVATE data region before requesting a WLC launch.
 * Computed against this node's own data region (data_devs[0]); intended for the
 * client role. Does not read the manager's assignment registry.
 */
typedef struct {
    __u32 chan_idx;         /* in:  compute channel index                 */
    __u32 reserved;
    __u64 userd_off;        /* out: USERD offset in the data region       */
    __u64 gpfifo_off;       /* out: GPFIFO ring offset in the data region */
    __u64 pushbuf_off;      /* out: method pushbuffer offset              */
    __u64 enc_off;          /* out: enc cipher-page offset (tag at +4096) */
} sev_gpu_ioctl_compute_info_t;

#define SEV_GPU_IOC_GET_COMPUTE_INFO \
    _IOWR(SEV_GPU_IOC_MAGIC, 27, sev_gpu_ioctl_compute_info_t)

#endif /* SEV_GPU_MANAGER_H */
