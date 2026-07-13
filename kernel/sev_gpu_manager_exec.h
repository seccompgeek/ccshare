/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_manager_exec.h — MANAGER GPU-execution path: CE secure-copy,
 * work submission, and channel-ownership validation. The manager is the sole
 * ringer of the real GPU; clients never submit directly. Ownership is checked
 * against the manager-authoritative assign_state before any GPU action.
 */
#ifndef SEV_GPU_MANAGER_EXEC_H
#define SEV_GPU_MANAGER_EXEC_H

#include <linux/types.h>

/* CE secure-copy submit exported by nvidia.ko (manager = sole ringer). */
typedef u32 (*sev_gpu_ce_submit_t)(u32 h_client, u32 flags,
				   u64 src, u64 dst, u64 length,
				   u64 auth_tag, u64 iv);
extern sev_gpu_ce_submit_t ce_submit_fn;

typedef u32 (*sev_gpu_submit_work_t)(u32 h_client, u32 h_channel);
extern sev_gpu_submit_work_t submit_work_fn;

/* module params used by the exec path (defined in sev_gpu_main.c). */
extern bool copy_loopback;
extern bool enforce_channel_ownership;

int  sev_gpu_do_ce_copy(u32 vm_id, u32 channel_id, u32 flags,
			u32 req_generation, u64 src_offset, u64 dst_offset,
			u64 length, u64 auth_tag_offset, u64 iv_offset);
bool sev_gpu_vm_owns_channel(u32 vm_id, u32 h_client, u32 h_channel);
int  sev_gpu_do_submit_work(u32 vm_id, u32 h_client, u32 h_channel,
			    bool trusted);

#endif /* SEV_GPU_MANAGER_EXEC_H */
