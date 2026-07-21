// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_manager_mmap.c — manager-side backing for client mmaps: shadow
 * doorbell GPA, osdesc carve/reset, manager-view phys->va. Registered into
 * nvidia.ko by rpc_manager_bind_nvidia().
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/io.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_manager_mmap.h"

/*
 * Manager: return the CLIENT's shadow doorbell GPA (within the client's
 * ivshmem compute reserve) for client @client_id.  The manager reads
 * client_mem_phys from the shared data header -- a u64 the client writes on
 * its first RPC -- and adds the doorbell-page offset.  Returns 0 if the
 * client has not yet published its GPA.
 */
u64 sev_gpu_shadow_db_impl(u32 client_id)
{
	struct sev_gpu_data_dev *dd;
	void __iomem *data_hdr;
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
	if (!client_phys) {
		data_hdr = sev_gpu_data_header_ptr(dd);
		if (!data_hdr)
			return 0;
		memcpy_fromio(&client_phys,
				      (u8 __iomem *)data_hdr +
				      offsetof(sev_gpu_data_header_t, client_mem_phys),
				      sizeof(client_phys));
	}

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
int sev_gpu_osdesc_carve_impl(u32 client_id, u64 size,
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
void sev_gpu_osdesc_reset_impl(u32 client_id)
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
unsigned long sev_gpu_shared_phys_to_va_impl(u64 phys, u64 size)
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
