/*
 * sev_gpu_rpc.h
 *
 * RM escape-ioctl forwarding protocol ("RM-RPC") for transparent GPU
 * API-remoting under SEV-SNP.
 *
 * MODEL (locked decision: transparent API-remoting):
 *   - An unmodified CUDA process runs on a CLIENT VM that has NO real GPU.
 *   - The client's nvidia.ko intercepts RM escape ioctls at the single seam
 *     in nvidia_ioctl() -- the call
 *         rm_ioctl(sp, nv, &nvlfp->nvfp, arg_cmd, arg_copy, arg_size)
 *     -- and instead forwards {arg_cmd, arg_copy, arg_size} (plus any nested
 *     param buffers) over ivshmem to the MANAGER VM, which permanently owns
 *     the real GPU.
 *   - The manager replays the identical rm_ioctl() against the real GPU using
 *     a per-client RM file-private (so each client gets an isolated RM handle
 *     namespace), then writes the (in/out) results back so the client can
 *     copy_to_user() them to the CUDA process unchanged.
 *
 * TRANSPORT:
 *   Each client already owns a hardware-isolated ivshmem-plain data region
 *   (see sev_gpu_data_header_t in sev_gpu_manager.h). We reuse that region:
 *
 *     [ 0                         ) sev_gpu_shmem_header_t
 *     [ SEV_GPU_DATA_HEADER_OFF   ) sev_gpu_data_header_t
 *     [ SEV_GPU_RPC_MAILBOX_OFF   ) sev_gpu_rpc_slot_t           (mailbox)
 *     [ SEV_GPU_RPC_STAGING_OFF   ) nested param-buffer staging  (bulk)
 *
 *   The top-level escape struct (always <= 4 KiB for every NVOSxx_PARAMETERS)
 *   travels INLINE in the mailbox. Nested pointers embedded in that struct
 *   (e.g. NVOS21_PARAMETERS.pAllocParms, NVOS54_PARAMETERS.params) are
 *   deep-copied into the staging area and described by the buffers[] table;
 *   the client rewrites each embedded pointer to a staging offset before
 *   posting, and the manager re-points them at its own staging mapping.
 *
 *   Synchronisation is NAPI-style, matching the existing data-plane: the
 *   poster flips `state` and rings the shared-control doorbell once; the peer
 *   polls `state` until it advances. Exactly one RM call is in flight per
 *   client at a time (the client serialises with a mutex), which is adequate
 *   for first bring-up; multi-slot rings can come later.
 */

#ifndef SEV_GPU_RPC_H
#define SEV_GPU_RPC_H

#include <linux/types.h>

#define SEV_GPU_RPC_MAGIC    0x52504347524d4756ULL  /* "RM-RPCGV" */
#define SEV_GPU_RPC_VERSION  1

/*
 * Mailbox + staging placement inside the per-client data region.
 * The mailbox starts after the shared metadata page; bulk nested-buffer
 * staging begins after the mailbox. Everything before
 * SEV_GPU_RPC_STAGING_OFF is control metadata; the staging area is the only
 * place large blobs live.
 */
#define SEV_GPU_RPC_MAILBOX_OFF   (4096UL)                 /* after metadata page */
#define SEV_GPU_RPC_MAILBOX_SIZE  (60UL * 1024UL)          /* one mailbox slot    */
#define SEV_GPU_RPC_STAGING_OFF   (SEV_GPU_RPC_MAILBOX_OFF + SEV_GPU_RPC_MAILBOX_SIZE)

/*
 * Per-VM RM-RPC slot region inside the control BAR (manager-side scheduling of
 * forwarded escapes). Each VM gets a SLOT_STRIDE-sized slot starting at
 * CTRL_REGION_OFF (3 MiB into the control BAR).
 */
#define SEV_GPU_RPC_CTRL_REGION_OFF  (3UL * 1024UL * 1024UL)  /* 3 MiB into control BAR */
#define SEV_GPU_RPC_SLOT_STRIDE      (8UL * 1024UL)           /* >= sizeof(slot), 4K-aligned */

/* Largest top-level escape struct we forward inline (covers all NVOSxx). */
#define SEV_GPU_RPC_INLINE_MAX    (4096U)

/* Maximum nested param buffers described per request. */
#define SEV_GPU_RPC_MAX_BUFFERS   16

/*
 * Zero-copy nested-param staging.
 *
 * Nested param buffers are NOT copied through the mailbox. The client writes
 * each pointee directly into a reserved window at the TOP of its own per-VM
 * ivshmem data region; the manager maps that same region and points the RM at
 * it in place (the RM runs the replay with PARAM_LOCATION_KERNEL, so it
 * reads/writes the pointee directly -- the manager copies nothing). The client
 * still pays the unavoidable copy_{from,to}_user to move bytes across the CUDA
 * process boundary, but the manager side is fully zero-copy and the per-request
 * size is bounded only by this window, not by the mailbox.
 *
 * The window is the last SEV_GPU_RPC_DATA_STAGING_SIZE bytes of the data region
 * (computed from the region size both peers map identically), so it never
 * collides with the CE-copy data-plane payload, which lives low in the region.
 */
#define SEV_GPU_RPC_DATA_STAGING_SIZE  (2UL * 1024UL * 1024UL)  /* 2 MiB at top of region */

/* Mailbox state machine (single in-flight request per client). */
#define SEV_GPU_RPC_IDLE     0u   /* no request pending                       */
#define SEV_GPU_RPC_REQUEST  1u   /* client posted a request; manager to run  */
#define SEV_GPU_RPC_REPLY    2u   /* manager posted the reply; client to read */

/* Direction of a nested param buffer relative to the RM call. */
#define SEV_GPU_RPC_BUF_IN     0x1u  /* copied client -> manager before call  */
#define SEV_GPU_RPC_BUF_OUT    0x2u  /* copied manager -> client after  call  */
#define SEV_GPU_RPC_BUF_INOUT  (SEV_GPU_RPC_BUF_IN | SEV_GPU_RPC_BUF_OUT)

/*
 * Descriptor for one nested param buffer that an escape struct points at.
 *
 *  client_ptr  - the ORIGINAL userspace pointer value found in the escape
 *                struct on the client. The client rewrites the in-struct
 *                pointer to `staging_off` before posting; the manager keeps
 *                client_ptr only so it can restore the original value in the
 *                outgoing reply (the CUDA process must see its own pointer).
 *  staging_off - byte offset, from the start of the per-VM DATA REGION, of this
 *                buffer's bytes in the zero-copy staging window. The manager
 *                turns this into the kernel address of its own mapping of that
 *                region (dd->mem + staging_off) and points the RM at it in
 *                place -- no copy on the manager side.
 *  size        - buffer length in bytes.
 *  dir         - SEV_GPU_RPC_BUF_* copy direction.
 *  struct_off  - byte offset, within the inline top-level escape struct, of
 *                the 8-byte pointer field that references this buffer (so the
 *                manager can patch it to its own staging address, and the
 *                client can restore it on reply). NV_RPC_STRUCT_OFF_NONE if
 *                the pointer lives inside another nested buffer instead.
 *  parent      - index into buffers[] of the buffer that contains this
 *                pointer field, or SEV_GPU_RPC_PARENT_TOPLEVEL if it lives in
 *                the inline top-level struct.
 */
#define SEV_GPU_RPC_STRUCT_OFF_NONE   0xffffffffu
#define SEV_GPU_RPC_PARENT_TOPLEVEL   0xffffffffu

typedef struct {
    uint64_t client_ptr;
    uint64_t staging_off;
    uint64_t size;
    uint32_t dir;
    uint32_t struct_off;
    uint32_t parent;
    uint32_t reserved;
} __attribute__((packed)) sev_gpu_rpc_buffer_t;

/*
 * The per-client RM-RPC mailbox. Lives at SEV_GPU_RPC_MAILBOX_OFF in the
 * client's isolated data region. Both peers map it; the C-bit is already
 * cleared on this region so plain stores are mutually visible.
 */
/*
 * Phase A mmap-context reply (A0). KIND mirrors the native classification the
 * driver would compute from access_start (IS_REG/IS_FB/IS_UD): the manager,
 * which ran the real RM, tells the client which kind this mapping is so the
 * client can build a matching context. mm_caching uses the NVOS33 caching enum
 * (see nvos.h); mm_prot uses NV_PROTECT_* (bit0=read, bit1=write).
 */
#define SEV_GPU_MM_KIND_NONE      0u
#define SEV_GPU_MM_KIND_DOORBELL  1u   /* HOPPER_USERMODE_A doorbell (UD aperture) */
#define SEV_GPU_MM_KIND_USERD     2u   /* channel USERD                            */
#define SEV_GPU_MM_KIND_GPFIFO    3u   /* GPFIFO ring                              */
#define SEV_GPU_MM_KIND_PUSHBUF   4u   /* pushbuffer                               */
#define SEV_GPU_MM_KIND_OSDESC    5u   /* os-described sysmem                       */
#define SEV_GPU_MM_KIND_PARAMS    6u   /* RM params/notifier sysmem (ctl node)     */
#define SEV_GPU_MM_KIND_REG       7u   /* register BAR aperture                    */

/* Caching (mirror of NVOS33_FLAGS_CACHING_TYPE_*, nvos.h). */
#define SEV_GPU_MM_CACHING_CACHED         0u
#define SEV_GPU_MM_CACHING_UNCACHED       1u
#define SEV_GPU_MM_CACHING_WRITECOMBINED  2u
#define SEV_GPU_MM_CACHING_DEFAULT        6u

/* HOPPER_USERMODE_A doorbell aperture size (NVC361_NV_USERMODE__SIZE = 64 KiB). */
#define SEV_GPU_MM_DOORBELL_SIZE          (64u * 1024u)

typedef struct {
    uint64_t magic;            /* SEV_GPU_RPC_MAGIC                          */
    uint32_t version;          /* SEV_GPU_RPC_VERSION                       */
    uint32_t client_vm_id;     /* owning client (matches data-region owner) */

    volatile uint32_t state;   /* SEV_GPU_RPC_*                             */
    uint32_t seq;              /* request sequence (debug / ordering)       */

    uint32_t cmd;              /* NV escape arg_cmd (NV_ESC_RM_ALLOC, ...)  */
    uint32_t arg_size;         /* size of the top-level escape struct       */

    int32_t  rm_status;        /* out: NV_STATUS returned by manager rm_ioctl */
    int32_t  ret;              /* out: 0 on success, -errno on transport err  */

    uint32_t n_buffers;        /* number of valid buffers[] entries          */
    uint32_t reserved0;

    /*
     * Client's data-region BAR2 GPA (pci_resource_start of the ivshmem-plain
     * device), written on every request so the manager can populate
     * client_mem_phys_cache[] even when the client has no data device mapped
     * in the data-header path.  0 if the client has no data device.
     */
    uint64_t client_data_phys;

    /*
     * Phase A (A0): mmap-context reply. On an intercepted NV_ESC_RM_MAP_MEMORY
     * the manager ran the REAL RM and knows facts the GPU-less client cannot
     * compute locally. It returns them here so the client driver can build a
     * genuine nv_alloc_mapping_context_t (mirroring rm_create_mmap_context) and
     * let native nvidia_mmap_helper classify + map correctly — replacing the
     * racy doorbell_mmap_pfn one-shot.
     *
     * Valid only when mm_valid != 0 on a MAP_MEMORY reply. All addresses are
     * the client's own ivshmem BAR2 GPA (shadow), not manager-side addresses.
     */
    uint32_t mm_valid;         /* 1 => the fields below are populated         */
    uint32_t mm_kind;          /* SEV_GPU_MM_KIND_* (doorbell/userd/...)      */
    uint64_t mm_shadow_gpa;    /* access_start: client BAR2 GPA of the mapping */
    uint64_t mm_size;          /* access_size: bytes                          */
    uint32_t mm_caching;       /* NVOS33_FLAGS_CACHING_TYPE_* (cached/uc/wc)  */
    uint32_t mm_prot;          /* NV_PROTECT_* (read / read-write)            */
    uint32_t mm_is_ctl;        /* 1 => mapping belongs on the ctl node        */
    uint32_t mm_reserved;

    sev_gpu_rpc_buffer_t buffers[SEV_GPU_RPC_MAX_BUFFERS];

    /* Top-level escape struct, copied in by client and out by manager. */
    uint8_t  inline_arg[SEV_GPU_RPC_INLINE_MAX];
} __attribute__((packed)) sev_gpu_rpc_slot_t;

/*
 * Synthetic RPC command: client asks the manager to program GPU page tables
 * for an external allocation that UVM is mapping. Not a real RM escape number;
 * the high bit marks it as an out-of-band SEV control message so the manager's
 * sev_gpu_rm_replay() dispatches it before reaching rm_ioctl().
 */
#define SEV_GPU_RPC_CMD_UVM_MAP_DMA  0x8001U

/*
 * Synthetic RPC command for UVM_MAP_DYNAMIC_PARALLELISM_REGION. The client
 * owns only the VA-range metadata; the manager installs/removes the special
 * SKED-reflected PTE in the client's real FERMI_VASPACE_A.
 */
#define SEV_GPU_RPC_CMD_UVM_SKED     0x8004U
#define SEV_GPU_RPC_UVM_SKED_MAP     1U
#define SEV_GPU_RPC_UVM_SKED_UNMAP   2U

/*
 * Synthetic RPC for client UVM managed ranges. The manager allocates protected
 * vidmem and maps it at the exact client-selected VA in the client's real RM
 * VA space before any client channel can submit work against the range.
 */
#define SEV_GPU_RPC_CMD_UVM_MANAGED       0x8005U
#define SEV_GPU_RPC_UVM_MANAGED_MAP       1U
#define SEV_GPU_RPC_UVM_MANAGED_UNMAP     2U

/*
 * Payload for SEV_GPU_RPC_CMD_UVM_MAP_DMA. Travels in the inline_arg field of
 * the mailbox slot (no nested buffers). The manager fills rm_status on reply.
 */
typedef struct {
    uint64_t gpu_va;    /* [IN]  GPU virtual address (from UVM_CREATE_EXTERNAL_RANGE) */
    uint64_t length;    /* [IN]  mapping length in bytes                              */
    uint64_t offset;    /* [IN]  byte offset into the physical allocation             */
    uint32_t hClient;   /* [IN]  RM client handle                                    */
    uint32_t hMemory;   /* [IN]  RM physical memory handle                           */
    uint32_t rm_status; /* [OUT] NV_STATUS from manager NV_ESC_RM_MAP_MEMORY_DMA    */
    uint32_t hVASpace;  /* [IN]  FERMI_VASPACE_A handle on manager (hDma for NVOS46)*/
} __attribute__((packed)) sev_gpu_rpc_uvm_map_dma_t;

typedef struct {
    uint64_t gpu_va;     /* [IN]  base of the one-page SKED-reflected range */
    uint64_t length;     /* [IN]  GPU page size / mapping length            */
    uint32_t hClient;    /* [IN]  RM namespace which owns hVASpace          */
    uint32_t hVASpace;   /* [IN]  target FERMI_VASPACE_A                    */
    uint32_t op;         /* [IN]  SEV_GPU_RPC_UVM_SKED_{MAP,UNMAP}          */
    uint32_t rm_status;  /* [OUT] manager NV_STATUS                         */
} __attribute__((packed)) sev_gpu_rpc_uvm_sked_t;

typedef struct {
    uint64_t gpu_va;     /* [IN]  managed range base                         */
    uint64_t length;     /* [IN]  managed range length                       */
    uint32_t hClient;    /* [IN]  RM namespace which owns hVASpace           */
    uint32_t hVASpace;   /* [IN]  target FERMI_VASPACE_A                     */
    uint32_t op;         /* [IN]  SEV_GPU_RPC_UVM_MANAGED_{MAP,UNMAP}        */
    uint32_t rm_status;  /* [OUT] manager NV_STATUS                          */
} __attribute__((packed)) sev_gpu_rpc_uvm_managed_t;

typedef int __sev_gpu_rpc_h_pad;

/* RPC nested-buffer descriptor + layout policy (impl in sev_gpu_main.c comm). */
struct rpc_nested {
	u32 ptr_off;
	u32 size;
	u32 dir;
};
int rpc_nested_layout(u32 cmd, const void *arg, u32 arg_size,
		      struct rpc_nested out[SEV_GPU_RPC_MAX_BUFFERS]);

#endif /* SEV_GPU_RPC_H */
