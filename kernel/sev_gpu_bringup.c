// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_bringup.c — autonomous bring-up watcher + shadow usermode PTIMER.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_manager_exec.h"
#include "sev_gpu_bringup.h"

static struct sev_gpu_bringup_watch bringup_watch[SEV_GPU_MAX_VMS];
static struct hrtimer usermode_timer;
static bool usermode_timer_armed;
static u32 bringup_ch[SEV_GPU_MAX_VMS][SEV_GPU_BRINGUP_MAX_CH][2]; /* [hClient,hChannel] */
static u32 bringup_nch[SEV_GPU_MAX_VMS];


/* Arm (or re-arm) the bring-up doorbell watch for a replay channel. */
void sev_gpu_bringup_arm(u32 vm, u32 h_client, u32 h_channel)
{
	struct sev_gpu_bringup_watch *w;

	if (vm >= SEV_GPU_MAX_VMS || !h_client || !h_channel)
		return;

	w = &bringup_watch[vm];
	w->h_client  = h_client;
	w->h_channel = h_channel;
	w->last_db   = 0;
	w->last_db_f = 0;
	w->rings     = 0;
	w->deadline  = jiffies + msecs_to_jiffies(SEV_GPU_BRINGUP_WATCH_MS);
	w->next_scan = jiffies;   /* scan on the first poll */
	WRITE_ONCE(w->active, true);

	pr_info("sev_gpu: bring-up watch ARMED VM%u hClient=0x%x hChannel=0x%x\n",
		vm, h_client, h_channel);

	/*
	 * Record the channel in the per-VM ring set (dedup). The shared-USERD
	 * channel that actually advances GP_PUT may have been armed earlier and
	 * overwritten in the single-slot watch above, so keep them all here.
	 */
	{
		u32 i, n = READ_ONCE(bringup_nch[vm]);

		for (i = 0; i < n && i < SEV_GPU_BRINGUP_MAX_CH; i++)
			if (bringup_ch[vm][i][0] == h_client &&
			    bringup_ch[vm][i][1] == h_channel)
				return;
		if (n < SEV_GPU_BRINGUP_MAX_CH) {
			bringup_ch[vm][n][0] = h_client;
			bringup_ch[vm][n][1] = h_channel;
			WRITE_ONCE(bringup_nch[vm], n + 1);
		}
	}
}

/* Disarm the watch (e.g. once the client starts issuing STAGED submissions). */
void sev_gpu_bringup_disarm(u32 vm)
{
	if (vm >= SEV_GPU_MAX_VMS)
		return;
	if (READ_ONCE(bringup_watch[vm].active)) {
		WRITE_ONCE(bringup_watch[vm].active, false);
		pr_info("sev_gpu: bring-up watch disarmed VM%u\n", vm);
	}
}

/*
 * Poll every armed watch once. Returns true if any watch is still active, so
 * the replay poller can shorten its idle sleep and sample the shadow doorbell
 * finely during the bring-up window (a plain memory write gives no wakeup).
 */
bool sev_gpu_bringup_poll(void)
{
	bool any = false;
	u32 vm;

	for (vm = 0; vm < SEV_GPU_MAX_VMS; vm++) {
		struct sev_gpu_bringup_watch *w = &bringup_watch[vm];
		struct sev_gpu_data_dev *dd;
		u64 db, o2;
		u32 tok, tok_f = 0, sem = 0;

		if (!READ_ONCE(w->active))
			continue;

		if (time_after(jiffies, w->deadline)) {
			WRITE_ONCE(w->active, false);
			pr_info("sev_gpu: bring-up watch expired VM%u (rings=%u)\n",
				vm, w->rings);
			continue;
		}

		dd = ((u32)vm < (u32)num_data_devs) ? data_devs[vm] : NULL;
		if (!dd || !dd->mem)
			continue;

		db = compute_doorbell_off(dd->mem_size);
		if (!db || db + 0xA0 > (u64)dd->mem_size)
			continue;

		any = true;

		/* Shadow usermode doorbell token the client's CUDA writes. */
		tok = ioread32((u8 __iomem *)dd->mem + db + 0x90);

		/*
		 * Diagnostic test: also sample the doorbell word of a possible
		 * SECOND usermode aperture at window offset 0xf090.  The
		 * +0xf080/+0xf084 fields seen live in the full-window scan look
		 * like a second TIME_0/TIME_1 pair, so if CUDA were ringing that
		 * aperture its work-submit token would land at +0xf090 rather
		 * than +0x90.  Read-only: bounds-guarded, logged on advance only,
		 * and never rung on (+0xf090 is not a real HW doorbell).
		 */
		if (db + 0xf094 <= (u64)dd->mem_size)
			tok_f = ioread32((u8 __iomem *)dd->mem + db + 0xf090);

		/*
		 * GP_PUT-advance doorbell ring (experiment). For each shared-USERD
		 * candidate (a >=2 MiB 0x3e carve), read GP_PUT@USERD+0x8C and
		 * GP_GET@USERD+0x88 (USERD at buf+0x2000). When GP_PUT advances past
		 * GP_GET the client has queued GPFIFO entries the GPU has not yet
		 * consumed -- ring every replay channel's real doorbell so the host
		 * re-reads the ring. Edge-triggered per candidate (lastput) to avoid
		 * repeated rings. Bounded GP_PUT sanity (<1024) rejects buffers that
		 * are pushbuffer/bounce data rather than a real USERD.
		 */
		{
			u32 c, nc = READ_ONCE(bringup_ncand[vm]);

			for (c = 0; c < nc && c < SEV_GPU_BRINGUP_MAX_CAND; c++) {
				u64 ub = bringup_userd_cand[vm][c] +
					 SEV_GPU_BRINGUP_USERD_IN_BUF;
				u32 gp_put, gp_get, k, nch;

				if (!bringup_userd_cand[vm][c] ||
				    ub + SEV_GPU_USERD_GP_PUT_OFF + 4 > (u64)dd->mem_size)
					continue;

				gp_put = ioread32((u8 __iomem *)dd->mem + ub +
						  SEV_GPU_USERD_GP_PUT_OFF);
				gp_get = ioread32((u8 __iomem *)dd->mem + ub +
						  SEV_GPU_USERD_GP_GET_OFF);

				/*
				 * Watch GP_GET independently of GP_PUT. After we
				 * ring the doorbell, GP_GET advancing toward
				 * GP_PUT is the ONLY proof the GPU host actually
				 * fetched and consumed the queued GPFIFO entries.
				 * If GP_GET never moves the ring did not reach the
				 * host (wrong token/channel not runnable); if it
				 * reaches GP_PUT the work ran and cuInit must be
				 * blocked on the completion/interrupt path instead.
				 */
				if (gp_get != bringup_cand_lastget[vm][c]) {
					pr_info("sev_gpu: bring-up GP_GET advance VM%u userd@0x%llx GP_GET %u -> %u (GP_PUT=%u)\n",
						vm, (unsigned long long)ub,
						bringup_cand_lastget[vm][c],
						gp_get, gp_put);
					bringup_cand_lastget[vm][c] = gp_get;
				}

				if (gp_put == 0 || gp_put >= 1024u ||
				    gp_put == gp_get ||
				    gp_put == bringup_cand_lastput[vm][c])
					continue;

				bringup_cand_lastput[vm][c] = gp_put;
				nch = READ_ONCE(bringup_nch[vm]);
				pr_info("sev_gpu: bring-up GP_PUT advance VM%u userd@0x%llx GP_GET=%u GP_PUT=%u (%u channel(s), ring=%d)\n",
					vm, (unsigned long long)ub,
					gp_get, gp_put, nch, bringup_ring);

				if (!bringup_ring) {
					/*
					 * Doorbell propagation disabled by module param
					 * (default is ON). The GP_PUT advance is still
					 * logged above for diagnostics.
					 */
					continue;
				}

				for (k = 0; k < nch && k < SEV_GPU_BRINGUP_MAX_CH; k++) {
					int rc = sev_gpu_do_submit_work(vm,
						bringup_ch[vm][k][0],
						bringup_ch[vm][k][1], true);

					pr_info("sev_gpu: bring-up GP_PUT ring VM%u hChannel=0x%x rc=%d\n",
						vm, bringup_ch[vm][k][1], rc);
					w->rings++;
				}
			}
		}

		/* Completion semaphore the client polls (0x3e pool + 0x3fffc). */
		o2 = READ_ONCE(osdesc_2m_off[vm]);
		if (o2 && o2 + 0x40000 <= (u64)dd->mem_size)
			sem = ioread32((u8 __iomem *)dd->mem + o2 + 0x3fffc);

		/*
		 * Broad diagnostic: the client's CUDA aborts on reading word
		 * 0x3fffc of its 0x3e control pool BEFORE it ever reaches the
		 * doorbell stage (no MAP_MEMORY of the usermode doorbell, no
		 * GPFIFO_SCHEDULE, +0x90 never written), so the +0x90 watch can't
		 * fire. The pool starts all-zero, so scan it for ANY non-zero
		 * dword: those are exactly the client's writes (USERD / GPFIFO /
		 * pushbuffer / GP_PUT it stages there). Their offsets reveal
		 * whether -- and where -- CUDA publishes work before bailing.
		 * Throttled; read-only.
		 */
		if (time_after_eq(jiffies, w->next_scan)) {
			u32 off, nz = 0, logged = 0;

			w->next_scan = jiffies +
				msecs_to_jiffies(SEV_GPU_BRINGUP_SCAN_MS);

			/*
			 * Scan the ENTIRE shadow usermode window (the full 64 KiB
			 * NV_VIRTUAL_FUNCTION region redirected from the *_USERMODE_A
			 * object -- clc761 BLACKWELL / clc661 HOPPER / clc361 VOLTA),
			 * not just the +0x90 doorbell word the ring-watch samples.
			 * The doorbell (NVC361_NOTIFY_CHANNEL_PENDING) is at window
			 * offset 0x90, in page 0, so it is covered either way; the
			 * wider sweep also catches any work-submit token the client
			 * writes elsewhere in the window. A populated 0x3e pushbuffer
			 * with rings=0 means CUDA staged work but never advanced the
			 * token. Logging every non-zero dword disambiguates "never
			 * rang" from "rang at an offset the +0x90 watch misses".
			 * Read-only.
			 */
			for (off = 0;
			     off + 4 <= SEV_GPU_COMPUTE_DOORBELL_PAGES * PAGE_SIZE &&
			     db + off + 4 <= (u64)dd->mem_size; off += 4) {
				u32 v = ioread32((u8 __iomem *)dd->mem + db + off);

				if (v)
					pr_info("sev_gpu: bring-up doorbell-scan VM%u db+0x%05x = 0x%08x\n",
						vm, off, v);
			}

			/*
			 * Sample the USERD ring pointers directly: GP_GET@0x88
			 * and GP_PUT@0x8C (NV_RAMUSERD_*). If GP_PUT has moved
			 * (esp. past GP_GET), CUDA queued a GPFIFO entry -- proof
			 * it published work regardless of whether the +0x90
			 * usermode doorbell ever fired (Hopper+/Blackwell CC
			 * channels kick via BAR0 0x30090, not the usermode page).
			 * userd_2m_off marks the first >=2 MiB carve, at whose head
			 * the replay-allocated USERD/GPFIFO lives. Read-only.
			 */
			{
				u64 u = READ_ONCE(userd_2m_off[vm]);

				if (u && u + SEV_GPU_USERD_GP_PUT_OFF + 4 <=
						(u64)dd->mem_size) {
					u32 gp_get = ioread32((u8 __iomem *)dd->mem +
							u + SEV_GPU_USERD_GP_GET_OFF);
					u32 gp_put = ioread32((u8 __iomem *)dd->mem +
							u + SEV_GPU_USERD_GP_PUT_OFF);

					if (gp_get || gp_put)
						pr_info("sev_gpu: bring-up USERD VM%u GP_GET@0x88=0x%08x GP_PUT@0x8C=0x%08x\n",
							vm, gp_get, gp_put);
				}
			}

			if (o2 && o2 + SEV_GPU_BRINGUP_SCAN_BYTES <=
					(u64)dd->mem_size) {
				for (off = 0; off < SEV_GPU_BRINGUP_SCAN_BYTES;
				     off += 4) {
					u32 v = ioread32((u8 __iomem *)dd->mem +
							 o2 + off);

					if (v) {
						nz++;
						if (logged < SEV_GPU_BRINGUP_SCAN_LOG) {
							pr_info("sev_gpu: bring-up scan VM%u 0x3e+0x%05x = 0x%08x\n",
								vm, off, v);
							logged++;
						}
					}
					if ((off & 0xffffu) == 0)
						cond_resched();
				}
				if (nz)
					pr_info("sev_gpu: bring-up scan VM%u total non-zero dwords=%u (of %u)\n",
						vm, nz,
						SEV_GPU_BRINGUP_SCAN_BYTES / 4);
			}
		}

		/*
		 * Report any advance of the 2nd-aperture (+0xf090) test word
		 * independently of +0x90 (which is dark), so this fires even when
		 * the primary doorbell never moves.
		 */
		if (tok_f != w->last_db_f) {
			pr_info("sev_gpu: bring-up watch VM%u doorbell +0xf090 0x%08x -> 0x%08x\n",
				vm, w->last_db_f, tok_f);
			w->last_db_f = tok_f;
		}

		if (tok == w->last_db)
			continue;	/* no advance since last sample */

		pr_info("sev_gpu: bring-up watch VM%u doorbell +0x90 0x%08x -> 0x%08x sem[0x3fffc]=0x%08x\n",
			vm, w->last_db, tok, sem);
		w->last_db = tok;

		if (tok != 0 && w->rings < SEV_GPU_BRINGUP_MAX_RINGS) {
			int rc = sev_gpu_do_submit_work(vm, w->h_client,
							w->h_channel, true);

			w->rings++;
			pr_info("sev_gpu: bring-up ring VM%u rc=%d (ring #%u)\n",
				vm, rc, w->rings);
		}
	}

	return any;
}

static void sev_gpu_usermode_timer_write(void)
{
	struct sev_gpu_data_dev *dd = data_devs[0];
	u8 __iomem *page;
	u64 off, ns;

	if (!dd || !dd->mem || !dd->mem_size)
		return;
	off = compute_doorbell_off(dd->mem_size);
	if (!off)
		return;

	ns = ktime_get_ns();
	page = (u8 __iomem *)dd->mem + off;

	/*
	 * Write TIME_1 (hi) before TIME_0 (lo).  The reader loop reads hi, lo,
	 * hi again and retries while the two hi reads disagree; publishing hi
	 * first means a reader that interleaves our two stores never
	 * reconstructs a low word that has wrapped past a hi word we have not
	 * yet bumped -- so it can never observe a backward jump at the ~4.29 s
	 * lo-rollover boundary.  Ordered by the UC ivshmem mapping.
	 */
	iowrite32((u32)(ns >> 32), page + SEV_GPU_USERMODE_TIME_1_OFF);
	iowrite32((u32)ns & SEV_GPU_USERMODE_TIME_0_MASK,
		  page + SEV_GPU_USERMODE_TIME_0_OFF);
}

static enum hrtimer_restart sev_gpu_usermode_timer_fn(struct hrtimer *t)
{
	sev_gpu_usermode_timer_write();
	hrtimer_forward_now(t, ns_to_ktime(SEV_GPU_USERMODE_TIMER_PERIOD_NS));
	return HRTIMER_RESTART;
}

void sev_gpu_usermode_timer_start(void)
{
	if (usermode_timer_armed)
		return;
	hrtimer_setup(&usermode_timer, sev_gpu_usermode_timer_fn,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	usermode_timer_armed = true;
	hrtimer_start(&usermode_timer,
		      ns_to_ktime(SEV_GPU_USERMODE_TIMER_PERIOD_NS),
		      HRTIMER_MODE_REL);
	pr_info("sev_gpu: USERMODE shadow GPU-timer emulation armed (period %u ns)\n",
		(unsigned int)SEV_GPU_USERMODE_TIMER_PERIOD_NS);
}

void sev_gpu_usermode_timer_stop(void)
{
	if (!usermode_timer_armed)
		return;
	hrtimer_cancel(&usermode_timer);
	usermode_timer_armed = false;
}