/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_client_mmap.h — CLIENT-side mmap redirect + shadow doorbell + osdesc.
 *
 * NVIDIA-NODE-CLASS (see nv-mmap.c nvidia_mmap_helper):
 *   /dev/nvidia0  (device node, !NV_IS_CTL_DEVICE): maps BAR/device memory.
 *       The doorbell is here, identified by IS_UD_OFFSET (access_start within
 *       the nv->ud usermode aperture), NOT by page offset. sev_gpu_shadow_db_impl
 *       returns the shadow of that usermode window (compute_doorbell_off + 0x90).
 *   /dev/nvidiactl (control node): maps RM SYSTEM memory (params/notifiers) —
 *       NEVER the doorbell. Such mappings must not consume the doorbell slot.
 *
 * TODO(correctness): sev_gpu_rm_forward currently treats any non-zero
 *   pLinearAddress as the doorbell. The precise test mirrors the driver's
 *   IS_UD_OFFSET (mapped object's phys in the usermode aperture). See the
 *   MAP_MEMORY reply handler in sev_gpu_client_rm.c.
 */
#ifndef SEV_GPU_CLIENT_MMAP_H
#define SEV_GPU_CLIENT_MMAP_H

#include <linux/types.h>

/* Client-mode: shadow doorbell PFN stored on MAP_MEMORY reply, consumed by the
 * mmap redirect. Single one-shot slot (see rm_forward TODO above). */
extern unsigned long doorbell_mmap_pfn;

int  sev_gpu_mmap_redirect_impl(u64 phys, u64 size, unsigned long *pfn_out);
int  sev_gpu_client_register_user_mmio(struct sev_gpu_dev *d);
void sev_gpu_client_unregister_user_mmio(struct sev_gpu_dev *d);
void mmap_client_bind_nvidia(void);
void mmap_client_unbind_nvidia(void);

#endif /* SEV_GPU_CLIENT_MMAP_H */
