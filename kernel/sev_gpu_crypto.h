/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_crypto.h — primitives: AEAD(AES-GCM), SHA256, HMAC, HKDF, ECDHE.
 * Pure crypto, transport-agnostic and role-agnostic. Promoted from static to
 * module-global so the handshake/KMB/seal layers can share them across files.
 */
#ifndef SEV_GPU_CRYPTO_H
#define SEV_GPU_CRYPTO_H

#include <linux/types.h>
#include <crypto/kpp.h>
#include "sev_gpu_manager.h"   /* SEV_GPU_COMM_KEY_LEN, KMB nonce/tag lens, sev_cc_kmb */

/* Handshake key-agreement sizes (P-256). Defined here so struct sev_gpu_ecdhe
 * and the crypto primitives can use them; the handshake .c also uses them. */
#define SEV_GPU_HS_PUBKEY_LEN	64	/* P-256 uncompressed x||y */
#define SEV_GPU_HS_SECRET_LEN	32	/* P-256 ECDH shared x     */
#define SEV_GPU_HS_NONCE_LEN	32
#define SEV_GPU_HS_CONFIRM_LEN	32	/* HMAC-SHA256 output      */
#define SEV_GPU_HS_CURVE	"ecdh-nist-p256"

/* Ephemeral ECDHE keypair handle (was defined inline in the .c). */
struct sev_gpu_ecdhe {
	struct crypto_kpp *tfm;
	u8 pub[SEV_GPU_HS_PUBKEY_LEN];
};

int  sev_gpu_aead(bool enc,
		  const u8 key[SEV_GPU_COMM_KEY_LEN],
		  const u8 nonce[SEV_GPU_KMB_NONCE_LEN],
		  const void *aad, unsigned int aad_len,
		  void *data, unsigned int data_len,
		  u8 tag[SEV_GPU_KMB_TAG_LEN]);
int  sev_gpu_kmb_fp(const struct sev_cc_kmb *kmb, u8 fp[8]);
int  sev_gpu_sha256(const u8 *data, unsigned int dlen, u8 out[32]);
int  sev_gpu_hmac_sha256(const u8 *key, unsigned int klen,
			 const u8 *data, unsigned int dlen, u8 out[32]);
int  sev_gpu_hkdf_expand32(const u8 prk[32], const char *label,
			   const u8 th[32], u8 out[32]);
int  sev_gpu_hs_derive(const u8 *pub_c, const u8 *nonce_c,
		       const u8 *pub_s, const u8 *nonce_s,
		       const u8 *z, const u8 *psk,
		       u8 comm_key[32], u8 confirm_key[32], u8 th_out[32]);
int  sev_gpu_ecdhe_init(struct sev_gpu_ecdhe *e);
int  sev_gpu_ecdhe_shared(struct sev_gpu_ecdhe *e,
			  const u8 peer_pub[SEV_GPU_HS_PUBKEY_LEN],
			  u8 out[SEV_GPU_HS_SECRET_LEN]);
void sev_gpu_ecdhe_free(struct sev_gpu_ecdhe *e);

#endif /* SEV_GPU_CRYPTO_H */
