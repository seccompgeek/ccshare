/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_handshake.h — ECDHE-PSK key agreement (TLS1.3 psk_dhe_ke-like) over
 * the shared TLS region. Establishes the per-client comm key + KMB channel.
 * Depends on crypto (ecdhe/hkdf/hmac/hs_derive), state (hs_mgr_state,
 * comm_keystore), and the transport accessor hs_ctrl_mailbox() (main side).
 */
#ifndef SEV_GPU_HANDSHAKE_H
#define SEV_GPU_HANDSHAKE_H

#include <linux/types.h>
#include <linux/workqueue.h>
#include "sev_gpu_manager.h"   /* SEV_GPU_TLS_REGION_OFF/STRIDE, MAX_VMS */
#include "sev_gpu_crypto.h"    /* HS pubkey/nonce/confirm lens, HS_CURVE */

/* ---- HS protocol constants (moved from the .c) ---- */
#define SEV_GPU_HS_MAGIC	0x31534b48u	/* "HKS1" */
#define SEV_GPU_HS_VERSION	1u
#define SEV_GPU_HS_IDLE		0u
#define SEV_GPU_HS_REQ		0x48534b31u	/* client posted a request */
#define SEV_GPU_HS_REPLY	0x48534b32u	/* manager posted the reply */
#define SEV_GPU_HS_MSG_HELLO	1u		/* {pub_c, nonce_c}   */
#define SEV_GPU_HS_MSG_FINISHED	2u		/* {confirm_c}        */
#define SEV_GPU_HS_MAX_ATTEMPTS	8
/* SEV_GPU_HS_CURVE lives in sev_gpu_crypto.h (shared with ecdhe_init). */

/* ---- on-wire handshake slot (shared TLS region) ---- */
typedef struct {
	__u32 magic;
	__u32 version;
	__u32 state;
	__u32 msg_type;
	__u32 status;
	__u32 reserved;
	__u8  pub_c[SEV_GPU_HS_PUBKEY_LEN];
	__u8  nonce_c[SEV_GPU_HS_NONCE_LEN];
	__u8  pub_s[SEV_GPU_HS_PUBKEY_LEN];
	__u8  nonce_s[SEV_GPU_HS_NONCE_LEN];
	__u8  confirm_s[SEV_GPU_HS_CONFIRM_LEN];
	__u8  confirm_c[SEV_GPU_HS_CONFIRM_LEN];
} sev_gpu_hs_slot_t;

struct sev_gpu_dev;   /* fwd */

/* handshake tunables/state (defined in sev_gpu_main.c). */
extern uint hs_keyspace;
extern uint hs_timeout_ms;
extern atomic_t hs_client_busy[SEV_GPU_MAX_VMS];
extern atomic_t hs_client_attempts[SEV_GPU_MAX_VMS];

/* Transport mailbox accessor — defined in sev_gpu_main.c. */
void __iomem *hs_ctrl_mailbox(u8 vm);

/* Handshake logic (moved to sev_gpu_handshake.c). */
void sev_gpu_commit_comm_key(u32 vm, const u8 key[SEV_GPU_COMM_KEY_LEN]);
void sev_gpu_hs_service_slot(u8 vm, void __iomem *hb);
int  sev_gpu_hs_client_run(u32 vm);
void sev_gpu_hs_work(struct work_struct *w);
/* wait_comm_key / hs_client_maybe_run are declared in sev_gpu_state.h. */

#endif /* SEV_GPU_HANDSHAKE_H */
