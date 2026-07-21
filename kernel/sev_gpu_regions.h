/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_regions.h — per-VM DATA region geometry (BAR2 layout).
 *
 * SINGLE SOURCE OF TRUTH for reserve offsets. Both roles (manager and client)
 * and both transports (ivshmem today, sev-channel later) MUST derive identical
 * region-relative offsets from the same mem_size, or the doorbell/USERD/reserve
 * addresses diverge between the two sides (the "wrong offset" bug class).
 *
 * All functions are pure (mem_size + compile-time constants only) so they are
 * safe to share as static inlines.
 *
 * NVIDIA-NODE-CLASS note: compute_doorbell_off() returns the shadow of the
 * *_USERMODE_A window (the /dev/nvidia0 device-node BAR aperture, IS_UD_OFFSET),
 * NOT a /dev/nvidiactl sysmem mapping. The doorbell is at window offset 0x90,
 * TIME_0/1 at 0x80/0x84, within the 64 KiB (16-page) VF window.
 */
#ifndef SEV_GPU_REGIONS_H
#define SEV_GPU_REGIONS_H

#include <linux/kernel.h>
#include <linux/mm.h>
#include "sev_gpu_rpc.h"      /* SEV_GPU_RPC_DATA_STAGING_SIZE */
#include "sev_gpu_manager.h"  /* SEV_GPU_MAX_CHANNELS_PER_VM, MAX_VMS */

static inline u64 rpc_staging_base(size_t mem_size)
{
	return (mem_size > SEV_GPU_RPC_DATA_STAGING_SIZE) ?
		(u64)mem_size - SEV_GPU_RPC_DATA_STAGING_SIZE : 0;
}

/*
 * Per-VM compute-channel reserve (L3.3). Each manager-assigned GR compute
 * channel needs three GPU-DMA-able pages in the assignee's PRIVATE data region:
 * page 0 = USERD (GP_PUT the client publishes), page 1 = the GPFIFO ring, and
 * page 2 = the method pushbuffer that GPFIFO entries point at. Two further pages
 * (page 3 = client-encrypted compute methods = the WLC decrypt source, page 4 =
 * that ciphertext's AES-GCM auth tag) back the Fork B GPU-autonomous launch;
 * they are unused by the plain Option-A (manager-rings-doorbell) datapath. They
 * are backed via OS_PHYS_ADDR in
 * nvidia.ko, so the addresses must be page-aligned and unprotected -- which the
 * ivshmem-plain DATA region (C-bit-clear) is. The reserve sits just below the
 * RPC staging window; like that window it is off-limits to CE bounce traffic.
 */
#define SEV_GPU_COMPUTE_CHAN_STRIDE    (5UL * PAGE_SIZE)   /* USERD + GPFIFO + pushbuf + enc(cipher+tag) */
/*
 * The client maps the WHOLE *_USERMODE_A object, which is the
 * NV_VIRTUAL_FUNCTION register window: dev_vm.h defines
 * NV_VIRTUAL_FUNCTION = 0x0003FFFF:0x00030000, i.e. DRF_SIZE == 0x10000 ==
 * 64 KiB (16 pages), NOT a single page. The doorbell the client writes is at
 * window offset 0x90 (BAR0 0x30090 = base 0x30000 + 0x90) and TIME_0/1 at
 * 0x80/0x84 -- all in page 0 -- but its mmap() still spans the full 64 KiB.
 * Back the entire window with dedicated shadow pages so the client's mapping
 * cannot overlap the adjacent RPC staging window.
 */
#define SEV_GPU_COMPUTE_DOORBELL_PAGES 16UL                /* shadow usermode VF window (64 KiB) */
#define SEV_GPU_COMPUTE_RESERVE_SIZE \
	((u64)SEV_GPU_MAX_CHANNELS_PER_VM * SEV_GPU_COMPUTE_CHAN_STRIDE + \
	 (u64)SEV_GPU_COMPUTE_DOORBELL_PAGES * PAGE_SIZE)

/*
 * Region-relative base offset of the compute reserve (page-aligned). Returns 0
 * if the region cannot host both the RPC staging window and the reserve.
 */
static inline u64 compute_reserve_base(size_t mem_size)
{
	u64 staging = rpc_staging_base(mem_size);

	if (staging <= SEV_GPU_COMPUTE_RESERVE_SIZE)
		return 0;
	return ALIGN_DOWN(staging - SEV_GPU_COMPUTE_RESERVE_SIZE, PAGE_SIZE);
}

/*
 * Region-relative offset of the shadow doorbell page.  It sits at the end of
 * the compute reserve, just past all per-channel USERD/GPFIFO/pushbuf slots.
 */
static inline u64 compute_doorbell_off(size_t mem_size)
{
	u64 base = compute_reserve_base(mem_size);

	if (!base)
		return 0;
	return base + (u64)SEV_GPU_MAX_CHANNELS_PER_VM * SEV_GPU_COMPUTE_CHAN_STRIDE;
}

/*
 * Per-VM OS-descriptor reserve. esc 0x27 with hClass
 * NV01_MEMORY_SYSTEM_OS_DESCRIPTOR registers a CUDA-owned CPU buffer as GPU
 * memory; faithful replay is impossible (the user VA lives in the client
 * process, and kernel RM rejects VA descriptors), so the manager backs each
 * such object with a slice of the client's ivshmem region instead. The reserve
 * sits just below the compute reserve and, like it, is off-limits to CE bounce
 * traffic. It is a monotonic bump pool (see sev_gpu_osdesc_carve_impl) rewound
 * only once the client frees its last carve, so it must hold the workload's
 * entire PEAK live working set within a single run. A full CUDA context bring-up
 * on Blackwell (sm_120) issues ~29 CC-secure channels plus several 2 MiB
 * shared-sysmem (esc-0x3e) buffers and a trailing ~8 MiB one, which peaks just
 * over 16 MiB and exhausted the old 16 MiB reserve (-ENOSPC -> NV_ERR_INSUFFICIENT
 * _RESOURCES on cudaMalloc). Size it at 32 MiB: ~2x the observed peak. The CE
 * pushbuffer reserve below this band further limits the low client-chosen CE
 * bounce range. NB: the client derives the same reserve geometry from this constant
 * (to keep bounce buffers clear of the pool), so both peers MUST build the same
 * value -- rebuild sev_gpu_manager.ko on the client and the manager together.
 */
#define SEV_GPU_OSDESC_RESERVE_SIZE	(32UL * 1024 * 1024)	/* 32 MiB */

/*
 * Region-relative base offset of the OS-descriptor reserve (page-aligned).
 * Returns 0 if the region cannot host it below the compute reserve.
 */
static inline u64 osdesc_reserve_base(size_t mem_size)
{
	u64 cbase = compute_reserve_base(mem_size);

	if (!cbase || cbase <= SEV_GPU_OSDESC_RESERVE_SIZE)
		return 0;
	return ALIGN_DOWN(cbase - SEV_GPU_OSDESC_RESERVE_SIZE, PAGE_SIZE);
}

/*
 * Per-VM WLC/LCIC reserve band (Fork B, Increment 3b step 2). The per-client
 * Work-Launch Channel pool's unprotected pool_sysmem (the encrypted run_push +
 * auth tags the GPU-less client writes) and the paired LCIC pool's pool_sysmem
 * (entry/exit tracking notifiers, also client-written) are backed zero-copy by
 * this client's ivshmem region so the client can write them directly and the
 * GPU DMAs them out of shared RAM. nvidia-uvm.ko imports each over its
 * manager-view GPA (mem_phys + offset) as an OS_PHYS_ADDR descriptor.
 *
 * The bands are TINY: nvidia-uvm sizes WLC pool_sysmem at WLC_SYSMEM_TOTAL_SIZE
 * (416 B) * num_channels and LCIC at sizeof(uvm_gpu_semaphore_notifier_t)
 * (4 B) * 2 * num_channels, with num_channels hard-capped at
 * UVM_CHANNEL_MAX_NUM_CHANNELS_PER_POOL (== UVM_PUSH_MAX_CONCURRENT_PUSHES ==
 * 16). So WLC needs <= 416*16 = 6656 B and LCIC <= 4*2*16 = 128 B. One page for
 * WLC comfortably holds the worst case; use four for headroom, one for LCIC.
 * nvidia-uvm re-validates size >= its exact requirement, so over-reserving here
 * is harmless. This band sits just BELOW the OS-descriptor reserve; like the
 * reserves above it, it is off-limits to CE bounce traffic. Because the unified
 * module derives this geometry identically on both peers, the client and
 * manager sev_gpu_manager.ko MUST be built from the same source (they always
 * are). Existing reserve offsets are UNCHANGED, so the osdesc bump pool and the
 * compute reserve keep their addresses.
 */
#define SEV_GPU_WLC_SYSMEM_SIZE		(4UL * PAGE_SIZE)	/* >= 416 * 16 ch */
#define SEV_GPU_LCIC_SYSMEM_SIZE	(1UL * PAGE_SIZE)	/* >= 4 * 2 * 16 ch */
#define SEV_GPU_WLC_LCIC_RESERVE_SIZE \
	(SEV_GPU_WLC_SYSMEM_SIZE + SEV_GPU_LCIC_SYSMEM_SIZE)

/*
 * Region-relative base offset of the per-VM WLC/LCIC reserve band
 * (page-aligned). The WLC pool_sysmem occupies the low SEV_GPU_WLC_SYSMEM_SIZE
 * bytes and the LCIC pool_sysmem the next SEV_GPU_LCIC_SYSMEM_SIZE bytes.
 * Returns 0 if the region cannot host the band below the OS-descriptor reserve.
 */
static inline u64 wlc_lcic_reserve_base(size_t mem_size)
{
	u64 obase = osdesc_reserve_base(mem_size);

	if (!obase || obase <= SEV_GPU_WLC_LCIC_RESERVE_SIZE)
		return 0;
	return ALIGN_DOWN(obase - SEV_GPU_WLC_LCIC_RESERVE_SIZE, PAGE_SIZE);
}

/*
 * Per-client CE pushbuffer reserve. UVM's normal CC pushbuffer has a protected
 * vidmem image and a UVM_PUSHBUFFER_SIZE (16 MiB) unprotected sysmem image. The
 * client CPU writes encrypted CE pushes into the latter and the manager GPU's
 * WLC reads those exact bytes, so a manager-local allocation is not sufficient.
 *
 * Keep the transport-side constant independent of nvidia-uvm headers, and let
 * nvidia-uvm validate it against UVM_PUSHBUFFER_SIZE when importing the band.
 * The band sits immediately below WLC/LCIC and therefore also below the OS
 * descriptor, compute, and RPC reserves.
 */
#define SEV_GPU_CE_PUSHBUFFER_SIZE	(16UL * 1024 * 1024)

static inline u64 ce_pushbuffer_reserve_base(size_t mem_size)
{
	u64 wbase = wlc_lcic_reserve_base(mem_size);

	if (!wbase || wbase <= SEV_GPU_CE_PUSHBUFFER_SIZE)
		return 0;
	return ALIGN_DOWN(wbase - SEV_GPU_CE_PUSHBUFFER_SIZE, PAGE_SIZE);
}

#endif /* SEV_GPU_REGIONS_H */
