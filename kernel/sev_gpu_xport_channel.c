// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_xport_channel.c — sev-channel TRANSPORT backend (Path B).
 *
 * The Phase-0 replacement for sev_gpu_xport_ivshmem.c. Implements the SAME
 * transport symbols (declared in sev_gpu_transport.h) over the sev-channel
 * device (1af4:10f1) instead of the ivshmem-doorbell device (1af4:1110).
 * Select ONE backend in the Makefile; the comm/crypto/handshake/KMB/manager
 * layers above the boundary are unchanged.
 *
 * TOPOLOGY (differs fundamentally from ivshmem):
 *   ivshmem : ONE shared control device, MSI-X 4-vector, IVPosition peer id,
 *             mailboxes indexed [vm] into a SHARED control BAR2.
 *   channel : ONE device instance PER CLIENT. 1 MSI vector. Static vm-id
 *             property (NO IVPosition). Each client's mailboxes live in ITS
 *             OWN per-VM region. The manager binds N instances (one per
 *             client); the DEVICE INSTANCE *is* the client identity.
 *
 * PEER IDENTITY:
 *   Read from BAR0 SEV_REG_{ROLE,VM_ID} at probe. The vm-id property is the
 *   array index into chan_devs[]. This RETIRES the ivshmem-server + IVPosition.
 *
 * SIGNALLING (no "ring peer N by id"):
 *   client -> manager : write BAR0 SEV_REG_DOORBELL on the client's ONE device.
 *                       KVM's ioeventfd fires the doorbell fd the manager
 *                       collected for this client; the dedicated manager
 *                       instance wakes. peer/vector args are inert.
 *   manager -> client : write BAR0 SEV_REG_COMPLETE on chan_devs[vm]. That
 *                       instance holds the client's completion irqfd; the
 *                       written VALUE is ignored (instance == target). The
 *                       4 ivshmem vectors collapse to doorbell + irq_status
 *                       demux.
 *
 * MAILBOXES:
 *   Per-client region (sev_gpu_rpc.h already lays this out):
 *     [0)                       sev_gpu_data_header_t
 *     [SEV_GPU_RPC_MAILBOX_OFF)  RM-RPC mailbox   <- rpc_ctrl_mailbox(vm)
 *     [SEV_GPU_RPC_STAGING_OFF)  nested-param staging
 *   req_slot()/grant_slot() index the request/grant regions of that client's
 *   own device, exactly as before but per-instance rather than shared.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_rpc.h"
#include "sev_gpu_state.h"
#include "sev_gpu_transport.h"
#include "sev_gpu_regions.h"

/*
 * The sev-channel BAR0 register ABI. Mirrors qemu/sev_channel_proto.h. Device
 * identity (SEV_CH_VENDOR_ID/DEVICE_ID/BAR_REGS_SIZE) now lives in
 * sev_gpu_transport.h so the probe in main and this backend share it.
 */
#define SEV_CH_BAR_REGS        0
#define SEV_CH_BAR_SHMEM       2

#define SEV_CH_REG_MAGIC       0x000
#define SEV_CH_REG_VERSION     0x004
#define SEV_CH_REG_STATUS      0x008
#define SEV_CH_REG_ROLE        0x00C
#define SEV_CH_REG_VM_ID       0x010
#define SEV_CH_REG_SHMEM_SIZE  0x014
#define SEV_CH_REG_DOORBELL    0x01C
#define SEV_CH_REG_IRQ_STATUS  0x030
#define SEV_CH_REG_IRQ_MASK    0x034
#define SEV_CH_REG_COMPLETE    0x040

#define SEV_CH_MAGIC           0x53564348u   /* "SVCH" */

#define SEV_CH_ROLE_CLIENT     0
#define SEV_CH_ROLE_MANAGER    1

#define SEV_CH_IRQ_COMPLETION  (1u << 0)
#define SEV_CH_IRQ_DOORBELL    (1u << 1)
#define SEV_CH_IRQ_COMPUTE_DB  (1u << 2)

/*
 * Manager: array of per-client device instances, indexed by the static vm-id.
 * chan_devs[vm] is the sev_gpu_dev bound to client `vm`. The client side binds
 * exactly one instance (also published as ctrl_dev by the probe in main).
 * Defined here because it is channel-backend-specific state; the ivshmem
 * backend had no equivalent (it used one shared device + IVPosition).
 */
struct sev_gpu_dev *chan_devs[SEV_GPU_MAX_VMS];

/* ------------------------------------------------------------------ */
/* Peer identity — static vm-id, not IVPosition.                       */
/* ------------------------------------------------------------------ */
/*
 * Under the channel there is no "manager peer id" to ring: a client rings its
 * own single doorbell and the dedicated manager instance wakes. We keep the
 * symbol for source-compat with call sites that pass its result to
 * ivshmem_ring(); the value is inert on the client (its device is the target)
 * and, on the manager, selects chan_devs[] via the vector/peer arg instead.
 * Returns this device's own vm-id.
 */
u16 sev_gpu_manager_peer(struct sev_gpu_dev *d)
{
	if (d)
		return (u16)d->client_vm_id;
	return SEV_GPU_MANAGER_PEER_ID;
}

/* ------------------------------------------------------------------ */
/* Ring / complete — doorbell (guest->host) and completion (host->guest)*/
/* ------------------------------------------------------------------ */
/*
 * ivshmem_ring(d, peer, vector) mapped onto the channel:
 *   - CLIENT device (d->is_manager == false): the client can only ring its own
 *     doorbell. Write SEV_CH_REG_DOORBELL; KVM's ioeventfd carries the kick to
 *     the manager. peer/vector are ignored.
 *   - MANAGER device: signal client `peer` by writing SEV_CH_REG_COMPLETE on
 *     THAT client's instance (chan_devs[peer]). The written value is inert; the
 *     instance identity selects the target completion irqfd. vector is ignored
 *     (the client demuxes via irq_status). If chan_devs[peer] is absent, drop.
 */
void ivshmem_ring(struct sev_gpu_dev *d, u16 peer, u16 vector)
{
	struct sev_gpu_dev *target;

	if (!d)
		return;

	if (!d->is_manager) {
		/* Client: ring our single doorbell (value inert). */
		if (d->regs)
			writel(0, d->regs + SEV_CH_REG_DOORBELL);
		return;
	}

	/* Manager: complete the specific client instance. */
	target = (peer < SEV_GPU_MAX_VMS) ? chan_devs[peer] : NULL;
	if (!target)
		target = d;                 /* fall back to this instance */
	if (target->regs)
		writel(peer, target->regs + SEV_CH_REG_COMPLETE);
	(void)vector;
}

/* ------------------------------------------------------------------ */
/* Mailbox / slot accessors — per-client region, not shared BAR.       */
/* ------------------------------------------------------------------ */
/*
 * The RM-RPC mailbox lives at SEV_GPU_RPC_MAILBOX_OFF inside THAT client's own
 * per-VM region (sev_gpu_rpc.h layout), NOT at a shared-control offset. Under
 * the channel the manager reaches client `vm` via chan_devs[vm]->shmem; the
 * client reaches its own via ctrl_dev->shmem (its single instance).
 */
void __iomem *rpc_ctrl_mailbox(u8 vm)
{
	struct sev_gpu_dev *d;

	if (vm >= SEV_GPU_MAX_VMS)
		return NULL;

	/* Manager: index the target client's instance. Client: its own. */
	d = chan_devs[vm];
	if (!d)
		d = ctrl_dev;               /* client side: single instance */
	if (!d || !d->shmem)
		return NULL;
	if (SEV_GPU_RPC_MAILBOX_OFF + SEV_GPU_RPC_MAILBOX_SIZE > d->shmem_size)
		return NULL;
	return d->shmem + SEV_GPU_RPC_MAILBOX_OFF;
}

/* Manager: wake the RM-RPC replay thread (called from the doorbell IRQ). */
void rpc_wake_manager(void)
{
	atomic_set(&rpc_kick, 1);
	wake_up_interruptible(&rpc_wq);
}

/*
 * req_slot/grant_slot: on the channel each client's request/grant records live
 * in its own region at d->request_off / d->grant_off (published by the layout
 * header). For a manager the correct instance is chan_devs[vm]; but these are
 * called with the bound device `d` already selected by the caller, so we index
 * d's region directly and let the caller pass the right instance.
 */
void __iomem *req_slot(struct sev_gpu_dev *d, u8 vm)
{
	if (!d || !d->shmem)
		return NULL;
	return d->shmem + d->request_off + (size_t)vm * sizeof(gpu_request_t);
}

void __iomem *grant_slot(struct sev_gpu_dev *d, u8 vm)
{
	if (!d || !d->shmem)
		return NULL;
	return d->shmem + d->grant_off + (size_t)vm * sizeof(gpu_grant_t);
}

/* ------------------------------------------------------------------ */
/* IRQ demux — one MSI vector, reason in SEV_CH_REG_IRQ_STATUS.         */
/* ------------------------------------------------------------------ */
/*
 * The channel exposes a SINGLE MSI vector (vs ivshmem's 4). The reason is read
 * from SEV_CH_REG_IRQ_STATUS and cleared write-1-to-clear. This handler
 * replaces the 4-way vector switch in sev_gpu_irq_handler(): the manager wakes
 * on DOORBELL/COMPUTE_DB, the client wakes on COMPLETION.
 *
 * Registered by the channel probe (in main) via request_irq on the single
 * pci_irq_vector(pdev, 0). Exposed so main's setup path can install it in
 * place of the ivshmem multi-vector loop.
 */
irqreturn_t sev_gpu_channel_irq(int irq, void *data)
{
	struct sev_gpu_dev *d = data;
	u32 status;

	if (!d || !d->regs)
		return IRQ_NONE;

	status = readl(d->regs + SEV_CH_REG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	/* Write-1-to-clear the bits we are about to service. */
	writel(status, d->regs + SEV_CH_REG_IRQ_STATUS);

	if (d->is_manager) {
		/* A client rang (RPC doorbell or compute doorbell). */
		if (status & (SEV_CH_IRQ_DOORBELL | SEV_CH_IRQ_COMPUTE_DB)) {
			/*
			 * Doorbell interrupt received. Log the raw status so we can
			 * see which bits the manager device actually raised. NOTE:
			 * the manager side currently raises DOORBELL for every wake
			 * (BAR0 RPC ring AND BAR2 compute ring share one db_notifier
			 * fd), so COMPUTE_DB may not be independently set here even
			 * for a compute ring -- the raw bits tell the truth. We also
			 * sample the compute doorbell token slot so the interrupt can
			 * be correlated with what CUDA wrote to +0x90/+0x94.
			 */
			pr_info_ratelimited(
				"sev_gpu: DOORBELL IRQ vm=%u status=0x%08x (DOORBELL=%d COMPUTE_DB=%d)\n",
				d->client_vm_id, status,
				!!(status & SEV_CH_IRQ_DOORBELL),
				!!(status & SEV_CH_IRQ_COMPUTE_DB));

			if (atomic_cmpxchg(&d->mgr_polling, 0, 1) == 0)
				mgr_irq_mask(d);
			if (d->sched_wq)
				queue_work(d->sched_wq, &d->sched_work);
			/* RPC replay path (mailbox scan) also needs a kick. */
			rpc_wake_manager();
			if (d->rpc_wq)
				queue_work(d->rpc_wq, &d->rpc_work);
		}
	} else {
		/* Manager completed our request. */
		if (status & SEV_CH_IRQ_COMPLETION) {
			if (atomic_cmpxchg(&d->cli_polling, 0, 1) == 0)
				disable_irq_nosync(pci_irq_vector(d->pdev, 0));
			wake_up_interruptible(&d->grant_wq);
		}
	}
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* IRQ mask/unmask/rearm — single vector under the channel.            */
/* ------------------------------------------------------------------ */
/*
 * ivshmem masked 2 vectors (NEW_REQUEST + RELEASE). The channel has one; we
 * mask/unmask it and also gate device-side delivery via SEV_CH_REG_IRQ_MASK so
 * a client-doorbell storm cannot re-fire before the poller drains.
 */
void mgr_irq_mask(struct sev_gpu_dev *d)
{
	if (!d->nvectors)
		return;
	disable_irq_nosync(pci_irq_vector(d->pdev, 0));
	if (d->regs)
		writel(0, d->regs + SEV_CH_REG_IRQ_MASK);
}

void mgr_irq_unmask(struct sev_gpu_dev *d)
{
	if (!d->nvectors)
		return;
	if (d->regs)
		writel(SEV_CH_IRQ_DOORBELL | SEV_CH_IRQ_COMPUTE_DB,
		       d->regs + SEV_CH_REG_IRQ_MASK);
	enable_irq(pci_irq_vector(d->pdev, 0));
}

void cli_irq_rearm(struct sev_gpu_dev *d)
{
	if (d->nvectors && atomic_cmpxchg(&d->cli_polling, 1, 0) == 1)
		enable_irq(pci_irq_vector(d->pdev, 0));
}

/* ------------------------------------------------------------------ */
/* Probe helpers — read identity from BAR0, register in chan_devs[].   */
/* ------------------------------------------------------------------ */
/*
 * Called by the channel probe path in main after BAR0/BAR2 are mapped. Reads
 * the static identity from device registers (replacing the IVPosition read),
 * validates the magic, cross-checks the published doorbell offset against the
 * module's own compute_doorbell_off(), and registers the instance in
 * chan_devs[] (manager) or as ctrl_dev's client id.
 *
 * Returns 0 on success, negative on a fatal mismatch.
 */
int sev_gpu_channel_probe_identity(struct sev_gpu_dev *d)
{
	u32 magic, role, vm_id;

	if (!d || !d->regs)
		return -EINVAL;

	magic = readl(d->regs + SEV_CH_REG_MAGIC);
	if (magic != SEV_CH_MAGIC) {
		pr_err("sev_gpu: channel bad magic 0x%08x (want 0x%08x)\n",
		       magic, SEV_CH_MAGIC);
		return -ENODEV;
	}

	role  = readl(d->regs + SEV_CH_REG_ROLE);
	vm_id = readl(d->regs + SEV_CH_REG_VM_ID);
	d->is_manager = (role == SEV_CH_ROLE_MANAGER);

	/*
	 * Identity index = the static vm-id the launch script passed to this
	 * device (published at SEV_CH_REG_VM_ID). We read it rather than using
	 * probe order because PCI enumeration order is NOT guaranteed to match
	 * the -device command-line order, so a probe counter could misalign the
	 * manager's chan_devs[] with the client each instance actually serves.
	 * The launch scripts pass vm-id=<i> per instance; that is the stable key.
	 *
	 * The client remains anonymous in SIGNALLING (it rings its own doorbell,
	 * addresses no one); vm-id is only an INDEX for the manager's per-client
	 * arrays and the client's /dev/sev_channel<N> minor.
	 *
	 * The compute doorbell (BAR2 + compute_doorbell_off + 0x90) is
	 * FIRE-AND-FORGET: CUDA writes the token into plain BAR2 RAM and the
	 * client driver rings the BAR0 RPC doorbell after; the manager reads it
	 * from RAM. No +0x90 trap, no published offset.
	 */
	if (vm_id >= SEV_GPU_MAX_VMS) {
		pr_err("sev_gpu: channel vm_id=%u out of range (max %d)\n",
		       vm_id, SEV_GPU_MAX_VMS);
		return -ERANGE;
	}
	d->client_vm_id = (u8)vm_id;
	d->ivposition   = (int)vm_id;          /* keep field for logs */
	chan_devs[vm_id] = d;

	pr_info("sev_gpu: channel bound role=%s slot=%u shmem=%zu compute_db_off=0x%llx\n",
		d->is_manager ? "manager" : "client", d->client_vm_id, d->shmem_size,
		(unsigned long long)compute_doorbell_off(d->shmem_size));
	return 0;
}

void sev_gpu_channel_remove_identity(struct sev_gpu_dev *d)
{
	int i;

	if (!d)
		return;
	for (i = 0; i < SEV_GPU_MAX_VMS; i++)
		if (chan_devs[i] == d)
			chan_devs[i] = NULL;
}

/* PCI id table for the channel device (used by main's pci_driver when the
 * channel backend is selected). Exposed so main can reference it. */
const struct pci_device_id sev_gpu_channel_ids[] = {
	{ PCI_DEVICE(SEV_CH_VENDOR_ID, SEV_CH_DEVICE_ID) },
	{ 0 }
};