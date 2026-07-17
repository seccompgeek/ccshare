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
#include "sev_gpu_rpc.h"
#include "sev_gpu_state.h"
#include "sev_gpu_client_mmap.h"

typedef void (*sev_gpu_register_mmap_redirect_t)(
	int (*fn)(u64 phys, u64 size, unsigned long *pfn_out));
typedef void (*sev_gpu_unregister_mmap_redirect_t)(void);

static sev_gpu_unregister_mmap_redirect_t mmap_unregister_fn;

extern void sev_gpu_register_mmap_redirect(
	int (*fn)(u64 phys, u64 size, unsigned long *pfn_out));
extern void sev_gpu_unregister_mmap_redirect(void);

/*
 * Phase A (A2): UD-window provider registration (nvidia.ko-exported).
 */
static void (*ud_window_unregister_fn)(void);
extern void sev_gpu_register_ud_window(int (*fn)(u64 *base, u64 *size));
extern void sev_gpu_unregister_ud_window(void);

/*
 * A2: report this client's shadow usermode-doorbell aperture so nvidia.ko can
 * set nv->ud. Base = client data-region BAR2 GPA + compute_doorbell_off; size =
 * the 64 KiB HOPPER_USERMODE_A window. Returns 0 on success, -errno if the
 * client's region isn't known yet (nvidia.ko then leaves nv->ud unset).
 */
int sev_gpu_ud_window_impl(u64 *base, u64 *size)
{
	struct sev_gpu_data_dev *dd;
	u64 off;

	if (!base || !size)
		return -EINVAL;
	dd = data_devs[0];
	if (!dd || !dd->mem_phys || !dd->mem_size)
		return -ENODEV;
	off = compute_doorbell_off(dd->mem_size);
	if (!off)
		return -ENODEV;
	*base = (u64)dd->mem_phys + off;
	*size = SEV_GPU_MM_DOORBELL_SIZE;
	return 0;
}






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

	/*
	 * Phase A (A2): register the UD-window provider so nvidia.ko can set
	 * nv->ud on the client (making native IS_UD_OFFSET classify the doorbell
	 * mapping). The window is this client's shadow usermode aperture:
	 * data_devs[0]->mem_phys + compute_doorbell_off(mem_size), 64 KiB.
	 */
	{
		void (*reg_ud)(int (*fn)(u64 *base, u64 *size)) =
			symbol_get(sev_gpu_register_ud_window);
		if (reg_ud) {
			ud_window_unregister_fn =
				symbol_get(sev_gpu_unregister_ud_window);
			reg_ud(sev_gpu_ud_window_impl);
			symbol_put(sev_gpu_register_ud_window);
			pr_info("sev_gpu: registered UD-window provider with nvidia.ko\n");
		}
	}
}

void mmap_client_unbind_nvidia(void)
{
	if (ud_window_unregister_fn) {
		ud_window_unregister_fn();
		symbol_put(sev_gpu_unregister_ud_window);
		ud_window_unregister_fn = NULL;
	}
	if (mmap_unregister_fn) {
		mmap_unregister_fn();
		symbol_put(sev_gpu_unregister_mmap_redirect);
		mmap_unregister_fn = NULL;
	}
}