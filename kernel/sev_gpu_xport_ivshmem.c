// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_xport_ivshmem.c — ivshmem-doorbell TRANSPORT backend.
 *
 * Implements the transport primitives declared in sev_gpu_transport.h over the
 * ivshmem-doorbell device (BAR0 registers + BAR2 shared region). This is the
 * SWAP POINT: a sev-channel backend (sev_gpu_xport_channel.c) would provide the
 * same symbols over the sev-channel device, and the Makefile would select one.
 *
 * Mechanism only. The IRQ *dispatch* (sev_gpu_irq_handler) and layout stamping
 * stay in sev_gpu_main.c because they encode manager/client scheduling policy.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/pci.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_rpc.h"
#include "sev_gpu_state.h"
#include "sev_gpu_transport.h"

/*
 * The RM-RPC mailbox lives in the SHARED CONTROL BAR, one fixed-stride slot per
 * VM (indexed like req_slot()/grant_slot()). Returns NULL if the control device
 * isn't bound or its BAR is too small to hold this VM's slot.
 */
void __iomem *rpc_ctrl_mailbox(u8 vm)
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
void rpc_wake_manager(void)
{
	atomic_set(&rpc_kick, 1);
	wake_up_interruptible(&rpc_wq);
}

void __iomem *req_slot(struct sev_gpu_dev *d, u8 vm)
{
	return d->shmem + d->request_off + (size_t)vm * sizeof(gpu_request_t);
}

void __iomem *grant_slot(struct sev_gpu_dev *d, u8 vm)
{
	return d->shmem + d->grant_off + (size_t)vm * sizeof(gpu_grant_t);
}

/* Ring a peer's doorbell vector (no-op if the device has no registers). */
void ivshmem_ring(struct sev_gpu_dev *d, u16 peer, u16 vector)
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
u16 sev_gpu_manager_peer(struct sev_gpu_dev *d)
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

/* Manager: mask/unmask the request + release doorbell vectors. */
void mgr_irq_mask(struct sev_gpu_dev *d)
{
	if (!d->nvectors)
		return;
	disable_irq_nosync(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_NEW_REQUEST));
	disable_irq_nosync(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_RELEASE));
}

void mgr_irq_unmask(struct sev_gpu_dev *d)
{
	if (!d->nvectors)
		return;
	enable_irq(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_NEW_REQUEST));
	enable_irq(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_RELEASE));
}

/* Client: re-enable the grant doorbell vector after a poll cycle. */
void cli_irq_rearm(struct sev_gpu_dev *d)
{
	if (d->nvectors && atomic_cmpxchg(&d->cli_polling, 1, 0) == 1)
		enable_irq(pci_irq_vector(d->pdev, IVSHMEM_VECTOR_GRANT_READY));
}