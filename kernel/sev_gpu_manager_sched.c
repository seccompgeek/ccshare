// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_manager_sched.c — manager scheduling, channel assignment, RPC replay.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_rpc.h"
#include "sev_gpu_comm.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_crypto.h"
#include "sev_gpu_kmb.h"
#include "sev_gpu_handshake.h"
#include "sev_gpu_nvidia.h"
#include "sev_gpu_manager_exec.h"
#include "sev_gpu_manager_sched.h"

/*
 * Greedy grant: scan request slots, grant any pending request, clear it, and
 * notify the requesting client. Round-robin/time-slicing can layer on top of
 * gpu_owner later.
 */
int sev_gpu_scan_and_grant(struct sev_gpu_dev *d)
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
/*
 * Service the independent compute doorbell for one client region.
 *
 * Edge-detects the header db_seq; on an advance, reads db_token and rings the
 * REAL GPU usermode doorbell with exactly that token via the nvidia.ko raw-ring
 * export. Touches ONLY db_seq/db_token -- never state/req_kind -- so it is fully
 * independent of the CE-copy / SUBMIT_WORK path. Returns 1 if it rang.
 */
int sev_gpu_doorbell_service(struct sev_gpu_data_dev *dd)
{
	void __iomem *hdr;
	u32 seq, token, rc;

	if (!dd || !dd->mem)
		return 0;
	if (dd->mem_size <= SEV_GPU_DATA_HEADER_SIZE)
		return 0;

	hdr = sev_gpu_data_header_ptr(dd);
	if (!hdr)
		return 0;

	seq = ioread32(hdr + offsetof(sev_gpu_data_header_t, db_seq));
	if (seq == dd->last_db_seq)
		return 0;			/* no new doorbell */

	rmb();
	token = ioread32(hdr + offsetof(sev_gpu_data_header_t, db_token));

	/* Consume this seq before ringing: a ring racing in during the RM call
	 * is caught next pass, never lost or double-served for the same seq. */
	dd->last_db_seq = seq;

	if (!ring_doorbell_fn) {
		pr_warn_ratelimited(
			"sev_gpu: doorbell vm=%u seq=%u token=0x%08x but nvidia.ko ring export absent\n",
			dd->client_vm_id, seq, token);
		return 0;
	}

	pr_info_ratelimited(
		"sev_gpu: DOORBELL mirror vm=%u seq=%u token=0x%08x -> ringing real GPU\n",
		dd->client_vm_id, seq, token);

	rc = ring_doorbell_fn(token);		/* -> sev_gpu_rm_ring_doorbell */
	if (rc != 0)
		pr_warn_ratelimited(
			"sev_gpu: doorbell ring vm=%u token=0x%08x rc=0x%x\n",
			dd->client_vm_id, token, rc);

	/*
	 * EXPERIMENT (doorbell-driven submit): the raw ring above uses db_token
	 * (which has been 0). The real compute channel stages GP_PUT in its shared
	 * USERD, detected only by the bring-up POLL today. Here we also drive a real
	 * work submission on the doorbell EDGE -- same do_submit_work the poll uses,
	 * but triggered the instant the client rings, which is the most native-like
	 * ordering (CUDA rings -> submit now). If GP_GET advances here but not from
	 * the poll, timing/ordering is the factor. We submit on every tracked compute
	 * channel for this VM (the real compute channel 0x5c0000d9 is among them).
	 */
	{
		u32 vm = dd->client_vm_id;
		u32 i, n;
		u32 hc = 0, hch = 0;

		n = sev_gpu_bringup_get_channels(vm, 0, NULL, NULL);
		for (i = 0; i < n; i++) {
			if (sev_gpu_bringup_get_channels(vm, i, &hc, &hch) == 0)
				break;
			if (!hc || !hch)
				continue;
			{
				int src = sev_gpu_do_submit_work(vm, hc, hch, true);
				pr_info_ratelimited(
					"sev_gpu: DOORBELL-submit vm=%u hClient=0x%x hChannel=0x%x rc=%d\n",
					vm, hc, hch, src);
			}
		}
	}

	return 1;
}

/*
 * EXPERIMENTAL full-page doorbell trap+replay: service one client's DB replay
 * ring slot. The client (QEMU) trapped a read/write on the doorbell page and is
 * spin-waiting; we perform the REAL VF-register access via db_mmio_fn and post
 * the result back, then flip state to DONE. Returns 1 if it serviced an op.
 *
 * Independent of the db_seq/token mirror path (which stays but is not hooked
 * when the client runs with doorbell_trap=on).
 */
int sev_gpu_dbring_service(struct sev_gpu_data_dev *dd)
{
	void __iomem *slot;
	u32 state, op, page_off, val, rd = 0xffffffffu, rc;
	u64 off64;

	if (!dd || !dd->mem)
		return 0;

	slot = dd->mem + sev_gpu_dbring_slot_off(dd->client_vm_id);

	state = ioread32(slot + offsetof(sev_gpu_dbring_slot_t, state));
	if (state != SEV_GPU_DBRING_REQ)
		return 0;			/* no pending op */

	rmb();
	op       = ioread32(slot + offsetof(sev_gpu_dbring_slot_t, op));
	off64    = ioread32(slot + offsetof(sev_gpu_dbring_slot_t, offset));
	off64   |= (u64)ioread32(slot + offsetof(sev_gpu_dbring_slot_t, offset) + 4) << 32;
	val      = ioread32(slot + offsetof(sev_gpu_dbring_slot_t, value));
	page_off = (u32)off64;

	/* High bit of op marks the real +0x90 compute doorbell; strip it. */
	{
		bool is_doorbell = (op & SEV_GPU_DBRING_OP_DOORBELL) != 0;
		op &= ~SEV_GPU_DBRING_OP_DOORBELL;
		if (is_doorbell)
			pr_info("sev_gpu: db-replay vm=%u COMPUTE DOORBELL ring off=0x%x val=0x%08x\n",
				dd->client_vm_id, page_off, val);
	}

	if (!db_mmio_fn) {
		pr_warn_ratelimited(
			"sev_gpu: db-replay vm=%u but nvidia.ko MMIO replay absent\n",
			dd->client_vm_id);
		iowrite32(0xffffffffu, slot + offsetof(sev_gpu_dbring_slot_t, rd_result));
		iowrite32(1u, slot + offsetof(sev_gpu_dbring_slot_t, status));
		wmb();
		iowrite32(SEV_GPU_DBRING_DONE,
			  slot + offsetof(sev_gpu_dbring_slot_t, state));
		return 1;
	}

	if (op == SEV_GPU_DBRING_OP_WRITE) {
		//@kymartin temporally forced
		rc = db_mmio_fn(1, page_off, val, NULL);
		pr_info_ratelimited(
			"sev_gpu: db-replay vm=%u WRITE off=0x%x val=0x%08x rc=0x%x\n",
			dd->client_vm_id, page_off, val, rc);
	} else {
		rc = db_mmio_fn(0, page_off, 0, &rd);
		pr_info_ratelimited(
			"sev_gpu: db-replay vm=%u READ  off=0x%x -> 0x%08x rc=0x%x\n",
			dd->client_vm_id, page_off, rd, rc);
	}

	iowrite32(rd, slot + offsetof(sev_gpu_dbring_slot_t, rd_result));
	iowrite32(rc ? 1u : 0u, slot + offsetof(sev_gpu_dbring_slot_t, status));
	wmb();
	iowrite32(SEV_GPU_DBRING_DONE,
		  slot + offsetof(sev_gpu_dbring_slot_t, state));
	return 1;
}

void sev_gpu_sched_work(struct work_struct *w)
{
	struct sev_gpu_dev *d = container_of(w, struct sev_gpu_dev, sched_work);
	int vm;

	if (!d->shmem)
		return;

	/*
	 * Independent compute-doorbell service. Runs on every wake, before grant
	 * scanning, and checks EVERY client region's db_seq: a client's compute
	 * doorbell shares the manager wake (one db_notifier fd) with RPC, so we
	 * cannot tell from the interrupt which client rang a compute doorbell --
	 * we simply scan all regions for an advanced db_seq and mirror the token.
	 * This is edge-based (one ring per bump) and touches only db_seq/db_token,
	 * never state/req_kind, so it is fully independent of the copy/submit path.
	 */
	for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
		struct sev_gpu_data_dev *dd = data_devs[vm];
		if (dd)
			sev_gpu_doorbell_service(dd);
	}

	/*
	 * EXPERIMENTAL full-page doorbell trap+replay. The client spin-waits on
	 * its DB replay-ring slot for each trapped read/write, so drain ALL VMs'
	 * slots repeatedly until none is pending -- a single wake may cover many
	 * back-to-back accesses (esp. PTIMER read loops). Bounded to avoid a
	 * livelock hog; if a client is hammering the doorbell the next wake picks
	 * up where we left off.
	 */
	if (db_mmio_fn) {
		int drained, rounds = 0;
		do {
			drained = 0;
			for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
				struct sev_gpu_data_dev *dd = data_devs[vm];
				if (dd)
					drained += sev_gpu_dbring_service(dd);
			}
		} while (drained && ++rounds < 4096);
	}

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
 * the same offsets from the shared reserve geometry. The CE pool imports its
 * 16 MiB unprotected pushbuffer from the adjacent CE reserve for the same reason.
 */
void sev_gpu_manager_setup_client_channels(u32 vm_id)
{
	uvm_sev_manager_create_client_pool_t create_fn;
	struct sev_gpu_data_dev *dd;
	u64 ce_band, ce_gpa, ce_size, band, wlc_gpa, wlc_size, lcic_gpa, lcic_size;
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
	ce_band = ce_pushbuffer_reserve_base(dd->mem_size);
	band = wlc_lcic_reserve_base(dd->mem_size);
	if (!ce_band || !band) {
		pr_warn("sev_gpu: VM %u data region too small for CE/WLC/LCIC reserves; per-client pools disabled\n",
			vm_id);
		return;
	}
	ce_gpa    = (u64)dd->mem_phys + ce_band;
	ce_size   = SEV_GPU_CE_PUSHBUFFER_SIZE;
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
	st = create_fn(manager_gpu_uuid, vm_id, ce_gpa, ce_size,
		       wlc_gpa, wlc_size, lcic_gpa, lcic_size);
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
void sev_gpu_manager_setup_work_fn(struct work_struct *work)
{
	unsigned vm_id;

	for (vm_id = 0; vm_id < SEV_GPU_MAX_VMS; vm_id++) {
		if (test_and_clear_bit(vm_id, &client_channels_pending))
			sev_gpu_manager_setup_client_channels(vm_id);
	}
}

/* Manager: drop the resident channel manager holds taken for every client whose
 * pool we created (balances the per-client retain). Called at module exit. */
void sev_gpu_manager_release_all_clients(void)
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
void register_vm(const sev_gpu_ioctl_register_vm_t *reg)
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

/* Service one client mailbox found in the SEV_GPU_RPC_REQUEST state. */
void sev_gpu_rpc_service(struct sev_gpu_data_dev *dd)
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
 * Manager: choose a channel for vm_id and stage its KMB into the assignment
 * registry. Pool mode (default) hands out a pre-provisioned channel of the
 * given keyspace; direct mode pins caller-supplied (manager-owned) handles;
 * with no GPU allocator a placeholder KMB keeps the seal/transport testable.
 * Fills *out_* (when non-NULL) with the manager's choice. The kmb_test
 * ASSIGN_CHANNEL ioctl and the automatic handshake worker share this path.
 */
int sev_gpu_assign_channel(u8 vm_id, u32 keyspace,
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
int sev_gpu_assign_compute_channel(u8 vm_id, u32 flags,
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
 * Service one client mediated-copy request found in a data region's header in
 * the SEV_GPU_DATA_STAGED state. The manager drives the GPU on the client's
 * behalf (it is the sole doorbell-ringer), publishes the status and flips the
 * state to SEV_GPU_DATA_DONE. @vm_id is the manager's pool index for this
 * region -- the trusted identity, NOT the client-written owner_vm_id.
 */
void sev_gpu_copy_service(struct sev_gpu_data_dev *dd, u32 vm_id)
{
	void __iomem *hdr = sev_gpu_data_header_ptr(dd);
	sev_gpu_data_header_t h;
	int rc;

	if (!hdr)
		return;

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

/* Manager: service one VM's transport-selected mailbox. */
void rpc_service_slot(u8 vm, sev_gpu_rpc_slot_t *slot)
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
	 * RM root lifetime normally ends when nvidiactl closes. The client mirrors
	 * that edge as FREE(root, root); once RM accepts it, discard only the
	 * process-owned doorbell/USERD watch state. Per-VM UVM pools and transport
	 * authentication deliberately survive for the next CUDA process.
	 */
	if (slot->cmd == RPC_NV_ESC_RM_FREE && size >= 16 &&
	    slot->rm_status == 0) {
		u32 hroot = 0, hold = 0, op_status = 0;

		memcpy(&hroot, slot->inline_arg + 0, 4);
		memcpy(&hold, slot->inline_arg + 8, 4);
		memcpy(&op_status, slot->inline_arg + 12, 4);
		if (hroot != 0 && hold == hroot && op_status == 0)
			sev_gpu_bringup_reset(vm);
	}

	/*
	 * SEV-DIAG: GET_WORK_SUBMIT_TOKEN (0xc36f0108) leaves a single 4-byte OUT
	 * param -- the token libcuda writes to the usermode doorbell (+0x90). Read
	 * it from the SHARED staging slot the RM just wrote in place: this is the
	 * exact byte the client copies back to CUDA. Compare with the RM-stored
	 * token (kernel_channel.c GET_WORK_SUBMIT_TOKEN trace) to see whether the
	 * store reached shared memory, and with the client's delivered-token print
	 * to see whether it survived the cross-VM read.
	 */
	if (slot->cmd == RPC_NV_ESC_RM_CONTROL && nested_ok && n >= 1 &&
	    dd && dd->mem && size >= RPC_NVOS54_CMD_OFF + 4 &&
	    slot->buffers[0].parent == SEV_GPU_RPC_PARENT_TOPLEVEL &&
	    slot->buffers[0].size >= 4 &&
	    slot->buffers[0].staging_off + 4 <= dd->mem_size) {
		u32 ctrl_cmd = 0;

		memcpy(&ctrl_cmd, slot->inline_arg + RPC_NVOS54_CMD_OFF, 4);
		if (ctrl_cmd == 0xc36f0108u) {
			u32 tok = 0;

			memcpy_fromio(&tok, (u8 __iomem *)dd->mem +
				      slot->buffers[0].staging_off, 4);
			pr_info("sev_gpu: GET_WORK_SUBMIT_TOKEN shared-staging token=0x%x staging_off=0x%llx rm_status=0x%x\n",
				tok,
				(unsigned long long)slot->buffers[0].staging_off,
				(u32)slot->rm_status);
		}
	}

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

	/*
	 * Phase A (A0): on a MAP_MEMORY reply, publish the mmap-context facts so
	 * the client can build a real fd-attached context (A1) instead of the
	 * doorbell one-shot. The driver's shadow-db hook already substituted the
	 * client-space shadow GPA into pLinearAddress (inline_arg+32); we read it
	 * back along with the REAL mapping length (inline_arg+24) and flags
	 * (inline_arg+44), then classify.
	 *
	 * NOT every intercepted MAP_MEMORY is the 64 KiB HOPPER_USERMODE_A doorbell.
	 * CUDA also maps the RM params/notification region on the ctl node (a larger,
	 * e.g. 2 MiB, mapping). Hard-coding doorbell size/kind makes
	 * nv_add_mapping_context_to_file() reject the later mmap with
	 * NV_ERR_INVALID_ARGUMENT (0x1f) because the context size (64 KiB) disagrees
	 * with the actual mmap length. So use the request's length, and derive kind
	 * from it: exactly the 64 KiB usermode window => DOORBELL (device node);
	 * anything else => PARAMS on the ctl node.
	 */
	slot->mm_valid = 0;
	if (slot->cmd == 0x4e /* NV_ESC_RM_MAP_MEMORY */ && slot->rm_status == 0) {
		u64 shadow_gpa = 0;
		u64 length = 0;
		u32 flags = 0;

		memcpy(&length,     slot->inline_arg + 24, 8); /* NVOS33 length     */
		memcpy(&shadow_gpa, slot->inline_arg + 32, 8); /* pLinearAddress    */
		memcpy(&flags,      slot->inline_arg + 44, 4); /* NVOS33 flags      */
		if (shadow_gpa && length) {
			bool is_doorbell = (length == SEV_GPU_MM_DOORBELL_SIZE);

			slot->mm_valid      = 1;
			slot->mm_kind       = is_doorbell ? SEV_GPU_MM_KIND_DOORBELL
							  : SEV_GPU_MM_KIND_PARAMS;
			slot->mm_shadow_gpa = shadow_gpa;
			slot->mm_size       = length;      /* the REAL mapping size */
			slot->mm_caching    = SEV_GPU_MM_CACHING_DEFAULT;
			/* NVOS33_FLAGS_ACCESS: 0=RW,1=RO,2=WO -> NV_PROTECT bits. */
			slot->mm_prot       = ((flags & 0x3u) == 1u) ? 0x1u  /* R  */
					     : ((flags & 0x3u) == 2u) ? 0x2u  /* W  */
					     :                          0x3u; /* RW */
			slot->mm_is_ctl     = is_doorbell ? 0u : 1u;
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
 * Manager replay poller. Scans every VM's transport-selected mailbox for pending
 * requests, services them, and otherwise sleeps until the RPC doorbell kicks it
 * or a short idle-poll timeout elapses (closes the kick-after-scan race, and
 * makes progress even if the doorbell lands on the wrong peer id).
 */
int rpc_thread_fn(void *unused)
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
		//watching = sev_gpu_bringup_poll();

		if (!did)
			wait_event_interruptible_timeout(rpc_wq,
				atomic_xchg(&rpc_kick, 0) || kthread_should_stop(),
				msecs_to_jiffies(watching ? SEV_GPU_BRINGUP_POLL_MS
							  : RPC_IDLE_POLL_MS));
	}

	kfree(slot);
	return 0;
}
