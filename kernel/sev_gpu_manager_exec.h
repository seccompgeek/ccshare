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

/* Independent compute-doorbell path: raw token -> real GPU doorbell.
 * Bound from nvidia.ko's sev_gpu_rm_ring_doorbell. Separate from submit_work. */
typedef u32 (*sev_gpu_ring_doorbell_t)(u32 token);
extern sev_gpu_ring_doorbell_t ring_doorbell_fn;
int sev_gpu_doorbell_service(struct sev_gpu_data_dev *dd);

/* EXPERIMENTAL full-page doorbell trap+replay: arbitrary-offset VF register
 * read/write. Bound from nvidia.ko's sev_gpu_rm_db_mmio. */
typedef u32 (*sev_gpu_db_mmio_t)(u32 is_write, u32 page_off, u32 in_val,
				 u32 *out_val);
extern sev_gpu_db_mmio_t db_mmio_fn;
int sev_gpu_dbring_service(struct sev_gpu_data_dev *dd);

/* module params used by the exec path (defined in sev_gpu_main.c). */
extern bool copy_loopback;
extern bool enforce_channel_ownership;

int  sev_gpu_do_ce_copy(u32 vm_id, u32 channel_id, u32 flags,
			u32 req_generation, u64 src_offset, u64 dst_offset,
			u64 length, u64 auth_tag_offset, u64 iv_offset);
bool sev_gpu_vm_owns_channel(u32 vm_id, u32 h_client, u32 h_channel);
int  sev_gpu_do_submit_work(u32 vm_id, u32 h_client, u32 h_channel,
			    bool trusted);

/* Doorbell-driven-submit experiment: iterate tracked compute channels for a VM.
 * Returns the count; fills h_client/h_channel for index i when i < count. */
u32  sev_gpu_bringup_get_channels(u32 vm, u32 i, u32 *h_client, u32 *h_channel);

#endif /* SEV_GPU_MANAGER_EXEC_H */