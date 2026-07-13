// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_manager_exec.c — manager GPU-execution: CE copy, submit, ownership.
 */
#include <linux/module.h>
#include <linux/io.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_manager_exec.h"

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
int sev_gpu_do_ce_copy(u32 vm_id, u32 channel_id, u32 flags,
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
bool sev_gpu_vm_owns_channel(u32 vm_id, u32 h_client, u32 h_channel)
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
int sev_gpu_do_submit_work(u32 vm_id, u32 h_client, u32 h_channel,
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