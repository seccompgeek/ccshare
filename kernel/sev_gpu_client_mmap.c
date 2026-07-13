// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_client_mmap.c — client mmap redirect, shadow doorbell, osdesc carve.
 * See sev_gpu_client_mmap.h for the /dev/nvidia0 vs /dev/nvidiactl distinction.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/io.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_client_mmap.h"

typedef void (*sev_gpu_register_mmap_redirect_t)(
	int (*fn)(u64 phys, u64 size, unsigned long *pfn_out));
typedef void (*sev_gpu_unregister_mmap_redirect_t)(void);

static sev_gpu_unregister_mmap_redirect_t mmap_unregister_fn;

extern void sev_gpu_register_mmap_redirect(
	int (*fn)(u64 phys, u64 size, unsigned long *pfn_out));
extern void sev_gpu_unregister_mmap_redirect(void);






int sev_gpu_mmap_redirect_impl(u64 phys, u64 size, unsigned long *pfn_out)
{
	struct sev_gpu_data_dev *dd;
	u64 reserve_base, reserve_end;

	if (!size || !IS_ALIGNED(phys, PAGE_SIZE))
		return -ENOENT;

	/*
	 * phys == 0: libcuda.so uses mmap(fd, offset=0) -- the normal NVIDIA
	 * convention where the kernel mmap context (stored by
	 * rm_create_mmap_context) carries the physical address.  We never call
	 * rm_create_mmap_context on the client, so we stash the shadow doorbell
	 * PFN in doorbell_mmap_pfn when the MAP_MEMORY reply arrives and serve
	 * it here as a one-shot.
	 */
	if (!phys) {
		unsigned long pfn = READ_ONCE(doorbell_mmap_pfn);
		if (pfn) {
			WRITE_ONCE(doorbell_mmap_pfn, 0);
			*pfn_out = pfn;
			pr_info("sev_gpu: mmap_redirect doorbell pfn=0x%lx\n", pfn);
			return 0;
		}
		pr_warn("sev_gpu: mmap_redirect: mmap offset=0 but no doorbell pfn stored\n");
		return -ENOENT;
	}

	dd = data_devs[0];
	if (!dd || !dd->mem_phys || !dd->mem_size)
		return -ENOENT;

	reserve_base = (u64)dd->mem_phys + compute_reserve_base(dd->mem_size);
	if (!reserve_base)
		return -ENOENT;
	reserve_end = reserve_base + SEV_GPU_COMPUTE_RESERVE_SIZE;

	pr_info("sev_gpu: mmap_redirect probe phys=0x%llx size=0x%llx"
		" reserve=[0x%llx,0x%llx)\n",
		(unsigned long long)phys, (unsigned long long)size,
		(unsigned long long)reserve_base, (unsigned long long)reserve_end);

	if (phys < reserve_base || phys + size > reserve_end) {
		pr_warn("sev_gpu: mmap_redirect MISS phys=0x%llx mem_phys=0x%llx"
			" reserve_base=0x%llx\n",
			(unsigned long long)phys,
			(unsigned long long)dd->mem_phys,
			(unsigned long long)reserve_base);
		return -ENOENT;
	}

	*pfn_out = phys >> PAGE_SHIFT;
	pr_info("sev_gpu: mmap redirect phys=0x%llx size=0x%llx pfn=0x%lx\n",
		(unsigned long long)phys, (unsigned long long)size, *pfn_out);
	return 0;
}

void mmap_client_bind_nvidia(void)
{
	sev_gpu_register_mmap_redirect_t reg;

	reg = symbol_get(sev_gpu_register_mmap_redirect);
	if (!reg) {
		pr_info("sev_gpu: nvidia mmap redirect absent; USERD mmap will fail until bound\n");
		return;
	}
	mmap_unregister_fn = symbol_get(sev_gpu_unregister_mmap_redirect);
	reg(sev_gpu_mmap_redirect_impl);
	symbol_put(sev_gpu_register_mmap_redirect);
	pr_info("sev_gpu: registered mmap redirect with nvidia.ko\n");
}

void mmap_client_unbind_nvidia(void)
{
	if (mmap_unregister_fn) {
		mmap_unregister_fn();
		symbol_put(sev_gpu_unregister_mmap_redirect);
		mmap_unregister_fn = NULL;
	}
}