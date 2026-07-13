/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_manager_sched.h — MANAGER scheduling, channel assignment, and the
 * RM-RPC replay service. The manager grants the GPU, assigns channels
 * (CE + compute), services client RPC mailboxes, and drives the replay thread.
 */
#ifndef SEV_GPU_MANAGER_SCHED_H
#define SEV_GPU_MANAGER_SCHED_H

#include <linux/types.h>
#include <linux/workqueue.h>
#include "sev_gpu_manager.h"   /* sev_gpu_ioctl_register_vm_t */
#include "sev_gpu_rpc.h"       /* sev_gpu_rpc_slot_t */

struct sev_gpu_dev;
struct sev_gpu_data_dev;

int  sev_gpu_scan_and_grant(struct sev_gpu_dev *d);
void sev_gpu_sched_work(struct work_struct *w);
void sev_gpu_manager_setup_client_channels(u32 vm_id);
void sev_gpu_manager_setup_work_fn(struct work_struct *work);
void sev_gpu_manager_release_all_clients(void);
void register_vm(const sev_gpu_ioctl_register_vm_t *reg);
void sev_gpu_rpc_service(struct sev_gpu_data_dev *dd);
int  sev_gpu_assign_channel(u8 vm_id, u32 keyspace,
			    u32 in_h_client, u32 in_h_channel,
			    u32 in_channel_id, u32 *out_channel_id,
			    u32 *out_h_client, u32 *out_h_channel);
int  sev_gpu_assign_compute_channel(u8 vm_id, u32 flags,
				    u32 *out_channel_id,
				    u32 *out_h_client, u32 *out_h_channel,
				    u64 *out_userd_off, u64 *out_gpfifo_off,
				    u64 *out_pushbuf_off, u64 *out_pushbuf_gpu_va);
void sev_gpu_copy_service(struct sev_gpu_data_dev *dd, u32 vm_id);
void rpc_service_slot(u8 vm, sev_gpu_rpc_slot_t *slot);
int  rpc_thread_fn(void *unused);

#endif /* SEV_GPU_MANAGER_SCHED_H */
