/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_nvidia.h — nvidia.ko binding contract: callback typedefs and the
 * function-pointer globals the manager binds via symbol_get at GPU bring-up.
 * Shared by manager exec/sched/assign and the bind/unbind glue in main.
 */
#ifndef SEV_GPU_NVIDIA_H
#define SEV_GPU_NVIDIA_H

#include <linux/types.h>

typedef u32 (*sev_gpu_kmb_fetch_t)(u32 h_client, u32 h_channel,
				   void *kmb_out, u32 kmb_size);
typedef u32 (*sev_gpu_chan_alloc_t)(u32 ce_id, u32 flags,
				    u32 *h_client, u32 *h_channel);
typedef u32 (*sev_gpu_chan_free_t)(u32 h_client);
typedef u32 (*sev_gpu_get_work_submit_token_t)(u32 h_client, u32 h_channel,
					       u32 *token);
typedef u32 (*sev_gpu_compute_alloc_t)(u32 flags, u64 userd_gpa, u64 gpfifo_gpa,
				       u64 pushbuf_gpa, u32 *h_client,
				       u32 *h_channel, u64 *pushbuf_gpu_va);
typedef u32 (*sev_gpu_compute_free_t)(u32 h_client);

/* Manager-bound nvidia.ko function pointers (defined in sev_gpu_main.c). */
extern sev_gpu_chan_alloc_t             chan_alloc_fn;
extern sev_gpu_chan_free_t              chan_free_fn;
extern sev_gpu_get_work_submit_token_t  get_work_submit_token_fn;
extern sev_gpu_compute_alloc_t          compute_alloc_fn;
extern sev_gpu_compute_free_t           compute_free_fn;


typedef u32 (*sev_gpu_rm_replay_t)(u32 client_id, u32 cmd, void *arg, u32 size);
typedef u32 (*uvm_sev_manager_create_client_pool_t)(const void *gpu_uuid, u32 client_id,
							    u64 ce_pushbuffer_gpa,
							    u64 ce_pushbuffer_size,
							    u64 wlc_gpa, u64 wlc_size,
							    u64 lcic_gpa, u64 lcic_size);
typedef void (*uvm_sev_manager_release_gpu_t)(void);
extern sev_gpu_rm_replay_t rpc_replay_fn;
extern sev_gpu_kmb_fetch_t kmb_fetch_fn;

/* nvidia-uvm symbols resolved via symbol_get (weak externs). */
extern u32 uvm_sev_manager_create_client_pool(const void *gpu_uuid, u32 client_id,
					      u64 ce_pushbuffer_gpa,
					      u64 ce_pushbuffer_size,
					      u64 wlc_gpa, u64 wlc_size,
					      u64 lcic_gpa, u64 lcic_size);
extern void uvm_sev_manager_release_gpu(void);

#endif /* SEV_GPU_NVIDIA_H */
