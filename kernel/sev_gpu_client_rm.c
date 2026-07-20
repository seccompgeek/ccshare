// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_client_rm.c — client RM interceptor (sev_gpu_rm_forward) + RPC client
 * call + nvidia forwarder binding. See sev_gpu_client_rm.h for the doorbell
 * (IS_UD_OFFSET) classification TODO.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_rpc.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_client_mmap.h"
#include "sev_gpu_comm.h"
#include "sev_gpu_client_rm.h"

/*
 * Phase A (A1): nvidia.ko-exported builder for a client-side fd-attached mmap
 * context. Resolved at runtime via symbol_get so the module still loads if an
 * older driver without this symbol is present (the call is then skipped).
 */
extern int nv_sev_build_mmap_context(u32 fd, u32 kind, u64 shadow_gpa,
				     u64 size, u32 caching, u32 prot,
				     u32 is_ctl);

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
u32 sev_gpu_rm_forward(u32 cmd, void *arg, u32 size)
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
	 * SEV-DIAG (notifier-mapping probe): an OS-descriptor alloc (esc 0x27,
	 * hClass NV01_MEMORY_SYSTEM_OS_DESCRIPTOR 0x71) is CUDA registering one of
	 * its OWN buffers -- e.g. a channel err-context, which is the
	 * WORK_SUBMIT_TOKEN notifier (RM writes info32 at +0x18). The manager backs
	 * the GPU side with a carve INSIDE this client's ivshmem region and writes
	 * the token there. Resolve CUDA's buffer page HERE (we run in CUDA's process
	 * context, so current->mm is CUDA's) and report whether it physically lives
	 * inside the ivshmem window [mem_phys, mem_phys+mem_size). If it does NOT,
	 * CUDA reads a private page the manager never touches -- the notifier write
	 * is invisible to CUDA. When in_ivshmem=1, 'off' must equal the manager's
	 * "osdesc carve ... off=0x..." for the same handle to be the SAME page.
	 */
	if (cmd == 0x27u /* NV_ESC_RM_ALLOC_MEMORY */ && arg && size >= 48 &&
	    dd && dd->mem_phys) {
		u32 osHClass = 0, osHandle = 0;
		u64 pMemory = 0, osLimit = 0;

		memcpy(&osHandle, (const u8 *)arg + 8, 4);   /* hObjectNew */
		memcpy(&osHClass, (const u8 *)arg + 12, 4);  /* hClass     */
		memcpy(&pMemory,  (const u8 *)arg + 24, 8);  /* pMemory VA */
		memcpy(&osLimit,  (const u8 *)arg + 32, 8);  /* limit      */

		if (osHClass == 0x71u && pMemory) {
			struct page *pg = NULL;
			long got = get_user_pages_fast(
					(unsigned long)pMemory & PAGE_MASK,
					1, 0, &pg);

			if (got == 1 && pg) {
				u64 phys = page_to_phys(pg) +
					   ((u64)pMemory & ~PAGE_MASK);
				u64 base = (u64)dd->mem_phys;
				u64 end  = base + (u64)dd->mem_size;
				int in_shm = (phys >= base && phys < end);

				pr_info("sev_gpu: osdesc-map-probe handle=0x%x userVA=0x%llx clientPhys=0x%llx in_ivshmem=%d off=0x%llx size=0x%llx\n",
					osHandle, (unsigned long long)pMemory,
					(unsigned long long)phys, in_shm,
					in_shm ? (unsigned long long)(phys - base)
					       : 0ULL,
					(unsigned long long)(osLimit + 1));
				put_page(pg);
			} else {
				pr_info("sev_gpu: osdesc-map-probe handle=0x%x userVA=0x%llx UNRESOLVED gup=%ld (special/PFNMAP mapping?)\n",
					osHandle, (unsigned long long)pMemory,
					got);
			}
		}
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
			 * Phase A (A1): build a REAL per-fd mmap context from the
			 * manager's reply, instead of the racy doorbell_mmap_pfn
			 * one-shot. The manager ran the real RM and returned the
			 * mapping facts in the slot's mm_* block (A0); we hand them
			 * to nvidia.ko's nv_sev_build_mmap_context so the later
			 * mmap(fd,off) resolves through the NATIVE nvidia_mmap_helper
			 * path (correct caching + IS_REG/IS_FB/IS_UD classification).
			 *
			 * The MAP_MEMORY escape (arg) is nv_ioctl_nvos33_parameters_
			 * with_fd = { NVOS33_PARAMETERS(48B); int fd; }, so the fd is
			 * at arg + 48. NVOS33 flags (access bits) are at arg + 44.
			 */
			u32 rm_status = 0;
			memcpy(&rm_status, slot->inline_arg + 40, 4); /* status */

			if (!rm_status && slot->mm_valid) {
				int fd = -1;
				int (*build)(u32, u32, u64, u64, u32, u32, u32);

				if (arg && size >= 52)
					memcpy(&fd, (const u8 *)arg + 48, 4); /* fd */

				build = symbol_get(nv_sev_build_mmap_context);
				WRITE_ONCE(doorbell_mmap_pfn,
					   (unsigned long)(slot->mm_shadow_gpa >> PAGE_SHIFT));
				if (build && fd >= 0) {
					int rc = build((u32)fd, slot->mm_kind,
						       slot->mm_shadow_gpa,
						       slot->mm_size,
						       slot->mm_caching,
						       slot->mm_prot,
						       slot->mm_is_ctl);
					pr_info("sev_gpu: MAP_MEMORY ctx fd=%d kind=%u "
						"gpa=0x%llx size=0x%llx rc=%d\n",
						fd, slot->mm_kind,
						(unsigned long long)slot->mm_shadow_gpa,
						(unsigned long long)slot->mm_size, rc);
				} else if (fd < 0) {
					pr_warn("sev_gpu: MAP_MEMORY reply: no fd in escape"
						" (size=%u); cannot build context\n", size);
				}
				if (build)
					symbol_put(nv_sev_build_mmap_context);
			} else {
				pr_warn("sev_gpu: MAP_MEMORY reply: mm_valid=%u"
					" rm_status=0x%x transport=0x%x\n",
					slot->mm_valid, rm_status, status);
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
 * Client: issue one RM-RPC request through our own private mailbox and block
 * for the reply. Serialized so only one request is in flight at a time. The
 * blob travels inline in the mailbox slot; the manager (loopback) echoes it.
 */
long sev_gpu_rpc_client_call(struct sev_gpu_dev *d, void __user *argp)
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