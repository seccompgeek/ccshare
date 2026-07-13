/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_transport.h — cross-VM TRANSPORT definitions and interface.
 *
 * This header is the SWAP BOUNDARY. Everything transport-specific (today: the
 * ivshmem-doorbell device) lives behind it. Code above this layer (comm,
 * crypto, handshake, KMB, manager scheduling/exec, client RM-forward) must not
 * reference IVSHMEM_* or d->regs directly — it goes through the sev_xport_*
 * accessors declared here and implemented in sev_gpu_xport_<backend>.c.
 *
 * Current backend:  sev_gpu_xport_ivshmem.c   (ivshmem-doorbell, 1af4:1110)
 * Future backend:   sev_gpu_xport_channel.c   (sev-channel: BAR2 shared file +
 *                                              ioeventfd doorbell + irqfd)
 *
 * To swap transports: implement this header in a new .c and change the
 * Makefile object list. The comm/crypto/handshake/KMB layers do not change.
 */
#ifndef SEV_GPU_TRANSPORT_H
#define SEV_GPU_TRANSPORT_H

/* ==================================================================== *
 *  IVSHMEM backend ABI (ivshmem-doorbell device: 1af4:1110)
 *  BAR0 = registers (IntrMask/IntrStatus/IVPosition/Doorbell)
 *  BAR2 = shared memory region (cross-VM)
 *  These constants are ivshmem-specific; the channel backend does not use
 *  them (it uses its own BAR layout in sev_channel_proto.h). They live here,
 *  not in sev_gpu_manager.h, so the transport boundary is explicit.
 * ==================================================================== */
#define IVSHMEM_VENDOR_ID 0x1af4
#define IVSHMEM_DEVICE_ID 0x1110

/* ivshmem BAR0 register offsets */
#define IVSHMEM_REG_INTRMASK   0x00  /* legacy INTx mask   (RW) */
#define IVSHMEM_REG_INTRSTATUS 0x04  /* legacy INTx status (RW) */
#define IVSHMEM_REG_IVPOSITION 0x08  /* this peer's ID     (RO) */
#define IVSHMEM_REG_DOORBELL   0x0C  /* ring a peer        (WO) */

/* Doorbell value = (peer_id << 16) | vector */
#define IVSHMEM_DOORBELL_VALUE(peer, vector) \
    (((uint32_t)((peer) & 0xffff) << 16) | ((vector) & 0xffff))

/* MSI-X vector semantics (doorbell) */
#define IVSHMEM_VECTOR_NEW_REQUEST 0  /* client -> manager: request queued   */
#define IVSHMEM_VECTOR_GRANT_READY 1  /* manager -> client: grant available  */
#define IVSHMEM_VECTOR_RELEASE     2  /* client -> manager: GPU released     */
#define IVSHMEM_VECTOR_RPC         3  /* RM-RPC mailbox kick (client<->mgr)  */
#define IVSHMEM_NUM_VECTORS        4

/* The manager is peer 0 by convention */
#define SEV_GPU_MANAGER_PEER_ID 0

/* ==================================================================== *
 *  Transport interface (the swap surface).
 *
 *  NOTE: during the initial refactor these are documented but the call
 *  sites still use the inline ivshmem primitives directly (behavior
 *  preserved). Step 3 of the refactor routes all call sites through these
 *  and moves the bodies into sev_gpu_xport_ivshmem.c. Signatures are stated
 *  here so the channel backend has a fixed contract to implement.
 *
 *  Kick / wake:
 *    void sev_xport_ring(struct sev_gpu_dev *d, u16 peer, u16 vector);
 *    void sev_xport_wake_manager(void);
 *
 *  Peer identity (ivshmem: IVPosition; channel: static vm-id):
 *    u16  sev_xport_peer(struct sev_gpu_dev *d);
 *
 *  IRQ plumbing:
 *    int  sev_xport_irq_setup(struct sev_gpu_dev *d);
 *    void sev_xport_irq_free(struct sev_gpu_dev *d);
 *    void sev_xport_irq_mask(struct sev_gpu_dev *d);
 *    void sev_xport_irq_unmask(struct sev_gpu_dev *d);
 *    void sev_xport_irq_rearm(struct sev_gpu_dev *d);
 *
 *  Shared-region mailbox accessors (return void __iomem * into the shared
 *  region for the given VM). Under ivshmem these index the control BAR2;
 *  under the channel they index that client's per-VM region.
 *    void __iomem *sev_xport_rpc_ctrl_mailbox(u8 vm);
 *    void __iomem *sev_xport_hs_ctrl_mailbox(u8 vm);
 *    void __iomem *sev_xport_kmb_mailbox(u8 vm);
 *    void __iomem *sev_xport_req_slot(struct sev_gpu_dev *d, u8 vm);
 *    void __iomem *sev_xport_grant_slot(struct sev_gpu_dev *d, u8 vm);
 *
 *  Layout publish/read (manager stamps the header, client reads it):
 *    void sev_xport_manager_init_layout(struct sev_gpu_dev *d);
 *    int  sev_xport_client_read_layout(struct sev_gpu_dev *d);
 * ==================================================================== */


/* ==================================================================== *
 *  Implemented in sev_gpu_xport_ivshmem.c (moved from the monolith).
 *  These are the ivshmem mechanism primitives; the channel backend will
 *  provide the same symbols over the sev-channel device.
 * ==================================================================== */
#ifndef __ASSEMBLY__
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/atomic.h>
struct sev_gpu_dev;

void ivshmem_ring(struct sev_gpu_dev *d, u16 peer, u16 vector);
u16  sev_gpu_manager_peer(struct sev_gpu_dev *d);
void rpc_wake_manager(void);
extern atomic_t rpc_kick;
extern wait_queue_head_t rpc_wq;

void __iomem *rpc_ctrl_mailbox(u8 vm);
void __iomem *req_slot(struct sev_gpu_dev *d, u8 vm);
void __iomem *grant_slot(struct sev_gpu_dev *d, u8 vm);

void mgr_irq_mask(struct sev_gpu_dev *d);
void mgr_irq_unmask(struct sev_gpu_dev *d);
void cli_irq_rearm(struct sev_gpu_dev *d);
#endif /* __ASSEMBLY__ */

#endif /* SEV_GPU_TRANSPORT_H */
