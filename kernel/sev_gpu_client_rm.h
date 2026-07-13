/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_client_rm.h — CLIENT-side RM interceptor + RPC client call.
 *
 * sev_gpu_rm_forward is the big RM-API interceptor: it forwards NV_ESC_RM_*
 * calls to the manager over the RPC mailbox and post-processes replies. This is
 * where MAP_MEMORY replies are classified.
 *
 * NVIDIA-NODE-CLASS / doorbell detection TODO:
 *   The MAP_MEMORY reply handler currently stores doorbell_mmap_pfn for any
 *   non-zero pLinearAddress. The precise test mirrors the NVIDIA driver's
 *   IS_UD_OFFSET: the doorbell is the /dev/nvidia0 usermode-aperture object
 *   (nv->ud range), NOT a /dev/nvidiactl sysmem map. Keying on the aperture
 *   (rather than "non-zero pLinearAddress") is the fix for the seven-vs-two
 *   mislabeling. See sev_gpu_client_mmap.h.
 */
#ifndef SEV_GPU_CLIENT_RM_H
#define SEV_GPU_CLIENT_RM_H

#include <linux/types.h>

/* nvidia.ko-registered RM forwarder. */
typedef u32 (*sev_gpu_rm_forwarder_t)(u32 cmd, void *arg, u32 size);

struct sev_gpu_dev;

u32  sev_gpu_rm_forward(u32 cmd, void *arg, u32 size);
long sev_gpu_rpc_client_call(struct sev_gpu_dev *d, void __user *argp);
void sev_gpu_client_attach_gpu(struct sev_gpu_dev *d);

#endif /* SEV_GPU_CLIENT_RM_H */
