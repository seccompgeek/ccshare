/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_manager_mmap.h — MANAGER-side backing for client mmaps: shadow
 * doorbell GPA, OS-descriptor carve/reset, and manager-view phys->va translate.
 * These run on the MANAGER and are registered into nvidia.ko by
 * rpc_manager_bind_nvidia(). (Distinct from the CLIENT mmap-redirect in
 * sev_gpu_client_mmap.c.)
 *
 * NVIDIA-NODE-CLASS: sev_gpu_shadow_db_impl returns the shadow of the
 * /dev/nvidia0 usermode-aperture (IS_UD_OFFSET) doorbell window; see
 * sev_gpu_client_mmap.h for the ctl-vs-device distinction.
 */
#ifndef SEV_GPU_MANAGER_MMAP_H
#define SEV_GPU_MANAGER_MMAP_H

#include <linux/types.h>

u64  sev_gpu_shadow_db_impl(u32 client_id);
int  sev_gpu_osdesc_carve_impl(u32 client_id, u64 size,
			       u64 *mgr_phys, u64 *cli_phys);
void sev_gpu_osdesc_reset_impl(u32 client_id);
unsigned long sev_gpu_shared_phys_to_va_impl(u64 phys, u64 size);

#endif /* SEV_GPU_MANAGER_MMAP_H */
