/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_kmb.h — Key Material Bundle transport: seal/install/send/recv.
 * Depends on crypto (aead/kmb_fp), state (comm_keystore), and the transport
 * mailbox accessor kmb_mailbox() (which stays on the main/transport side
 * because it resolves the transport mailbox).
 */
#ifndef SEV_GPU_KMB_H
#define SEV_GPU_KMB_H

#include <linux/types.h>
#include "sev_gpu_manager.h"   /* kmb ABI: REGION_OFF/SLOT_STRIDE, sev_gpu_kmb_mbox, sev_cc_kmb */

/* nvidia.ko-registered KMB callback signatures (moved from the .c). */
typedef u32 (*sev_gpu_kmb_seal_t)(u32 client_id, u32 channel_id,
				  const void *kmb_plain, u32 kmb_len,
				  void *out_nonce, void *out_tag, void *out_ct,
				  u32 *out_seq, u32 *out_keyspace);
typedef u32 (*sev_gpu_kmb_install_t)(u32 channel_id, u32 seq, u32 keyspace,
				     const void *nonce, const void *tag,
				     const void *ct, u32 ct_len, void *kmb_out);


/* KMB additional-authenticated-data (AAD) header (was inline in the .c). */
struct sev_gpu_kmb_aad {
	__u32 magic;
	__u32 vm_id;
	__u32 channel_id;
	__u32 keyspace;
	__u32 seq;
} __packed;

struct sev_gpu_dev;   /* fwd */

/* Transport mailbox accessor — defined in sev_gpu_main.c. */
void __iomem *kmb_mailbox(u8 vm);
extern atomic_t sev_gpu_kmb_pull_seq;

/* KMB logic (moved to sev_gpu_kmb.c). */
int sev_gpu_send_kmb(u8 vm_id, u32 channel_id, unsigned int to_ms,
		     u8 fp_out[8]);
int sev_gpu_recv_kmb(struct sev_gpu_dev *d, unsigned int to_ms,
		     u32 *out_channel_id, u32 *out_keyspace, u8 fp_out[8]);
u32 sev_gpu_kmb_seal_impl(u32 client_id, u32 channel_id,
			  const void *kmb_plain, u32 kmb_len,
			  void *out_nonce, void *out_tag, void *out_ct,
			  u32 *out_seq, u32 *out_keyspace);
u32 sev_gpu_kmb_install_impl(u32 channel_id, u32 seq, u32 keyspace,
			     const void *nonce, const void *tag,
			     const void *ct, u32 ct_len, void *kmb_out);

#endif /* SEV_GPU_KMB_H */
