/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_bringup.h — autonomous CUDA bring-up watcher + shadow usermode timer.
 *
 * Watches the shared-USERD GP_PUT (+0x8C) advance and the usermode doorbell
 * (+0x90) for the non-secure GR channel, and (default ON) propagates a
 * transparent-replay channel's doorbell ring. The usermode timer emulates the
 * GPU PTIMER (TIME_0/1 at +0x80/0x84) into the shadow doorbell page so a client
 * polling PTIMER sees it advance. See sev_gpu_client_mmap.h for the
 * /dev/nvidia0 usermode-aperture (IS_UD_OFFSET) context.
 */
#ifndef SEV_GPU_BRINGUP_H
#define SEV_GPU_BRINGUP_H

#include <linux/types.h>
#include "sev_gpu_manager.h"

/* USERD ring-pointer offsets (clc86f control struct). */
#define SEV_GPU_USERD_GP_GET_OFF   0x88u
#define SEV_GPU_USERD_GP_PUT_OFF   0x8Cu

/* Shadow usermode PTIMER (NVC361 TIME_0/1). */
#define SEV_GPU_USERMODE_TIME_0_OFF	0x80u
#define SEV_GPU_USERMODE_TIME_1_OFF	0x84u
#define SEV_GPU_USERMODE_TIME_0_MASK	0xffffffe0u
#define SEV_GPU_USERMODE_TIMER_PERIOD_NS	(20u * NSEC_PER_USEC)

#define SEV_GPU_BRINGUP_WATCH_MS   8000u  /* max window to watch after arming   */
#define SEV_GPU_BRINGUP_MAX_RINGS    64u  /* cap rings so a stuck token can't spin */
#define SEV_GPU_BRINGUP_SCAN_BYTES 0x200000u /* scan the full 0x3e pool (2 MiB) */
#define SEV_GPU_BRINGUP_SCAN_MS     1000u /* throttle the heavy pool scan       */
#define SEV_GPU_BRINGUP_SCAN_LOG       8  /* max non-zero dwords logged per scan */
#define SEV_GPU_BRINGUP_MAX_CH       64u
#define SEV_GPU_BRINGUP_USERD_IN_BUF 0x2000u /* USERD offset inside a shared buf */

struct sev_gpu_bringup_watch {
	bool          active;
	u32           h_client;
	u32           h_channel;
	u32           last_db;
	u32           last_db_f;
	u32           rings;
	unsigned long deadline;
	unsigned long next_scan;
};

/* arm/disarm/poll are declared in sev_gpu_state.h (called from sched/main). */
void sev_gpu_usermode_timer_start(void);
void sev_gpu_usermode_timer_stop(void);
extern bool bringup_ring;

#endif /* SEV_GPU_BRINGUP_H */
