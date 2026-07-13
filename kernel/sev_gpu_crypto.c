// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_crypto.c — AEAD/SHA/HMAC/HKDF/ECDHE primitives (moved verbatim from
 * sev_gpu_manager.c; static -> global, ecdhe struct -> sev_gpu_crypto.h).
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <crypto/aead.h>
#include <crypto/hash.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_crypto.h"

int sev_gpu_aead(bool enc,
			const u8 key[SEV_GPU_COMM_KEY_LEN],
			const u8 nonce[SEV_GPU_KMB_NONCE_LEN],
			const void *aad, unsigned int aad_len,
			void *data, unsigned int data_len,
			u8 tag[SEV_GPU_KMB_TAG_LEN])
{
	struct crypto_aead *tfm;
	struct aead_request *req;
	struct scatterlist sg;
	DECLARE_CRYPTO_WAIT(wait);
	unsigned int buf_len;
	u8 *buf;
	int ret;

	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	ret = crypto_aead_setkey(tfm, key, SEV_GPU_COMM_KEY_LEN);
	if (ret)
		goto out_tfm;
	ret = crypto_aead_setauthsize(tfm, SEV_GPU_KMB_TAG_LEN);
	if (ret)
		goto out_tfm;

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto out_tfm;
	}

	/* One contiguous buffer holding [aad][data][tag]. */
	buf_len = aad_len + data_len + SEV_GPU_KMB_TAG_LEN;
	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_req;
	}
	if (aad_len)
		memcpy(buf, aad, aad_len);
	memcpy(buf + aad_len, data, data_len);
	if (!enc)
		memcpy(buf + aad_len + data_len, tag, SEV_GPU_KMB_TAG_LEN);

	sg_init_one(&sg, buf, buf_len);
	aead_request_set_callback(req,
				  CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				  crypto_req_done, &wait);
	aead_request_set_ad(req, aad_len);

	if (enc) {
		aead_request_set_crypt(req, &sg, &sg, data_len, (u8 *)nonce);
		ret = crypto_wait_req(crypto_aead_encrypt(req), &wait);
		if (!ret) {
			memcpy(data, buf + aad_len, data_len);
			memcpy(tag, buf + aad_len + data_len, SEV_GPU_KMB_TAG_LEN);
		}
	} else {
		aead_request_set_crypt(req, &sg, &sg,
				       data_len + SEV_GPU_KMB_TAG_LEN, (u8 *)nonce);
		ret = crypto_wait_req(crypto_aead_decrypt(req), &wait);
		if (!ret)
			memcpy(data, buf + aad_len, data_len);
	}

	memzero_explicit(buf, buf_len);
	kfree(buf);
out_req:
	aead_request_free(req);
out_tfm:
	crypto_free_aead(tfm);
	return ret;
}

/* First 8 bytes of SHA-256(KMB): a stable fingerprint for the self-test. */
int sev_gpu_kmb_fp(const struct sev_cc_kmb *kmb, u8 fp[8])
{
	struct crypto_shash *tfm;
	u8 digest[32];
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, (const u8 *)kmb,
					  sizeof(*kmb), digest);
		shash_desc_zero(desc);
	}
	if (!ret)
		memcpy(fp, digest, 8);
	memzero_explicit(digest, sizeof(digest));
	crypto_free_shash(tfm);
	return ret;
}


/* SHA-256 of a single contiguous buffer. */
int sev_gpu_sha256(const u8 *data, unsigned int dlen, u8 out[32])
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, data, dlen, out);
		shash_desc_zero(desc);
	}
	crypto_free_shash(tfm);
	return ret;
}

/* HMAC-SHA256(key, data) -> out[32]. */
int sev_gpu_hmac_sha256(const u8 *key, unsigned int klen,
			       const u8 *data, unsigned int dlen, u8 out[32])
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	ret = crypto_shash_setkey(tfm, key, klen);
	if (!ret) {
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, data, dlen, out);
		shash_desc_zero(desc);
	}
	crypto_free_shash(tfm);
	return ret;
}

/* HKDF-Expand (single 32-byte block): T = HMAC(prk, label || th || 0x01). */
int sev_gpu_hkdf_expand32(const u8 prk[32], const char *label,
				 const u8 th[32], u8 out[32])
{
	u8 info[64 + 32 + 1];
	unsigned int llen = (unsigned int)strlen(label);
	unsigned int n = 0;

	if (llen > 64)
		return -EINVAL;
	memcpy(info, label, llen);
	n += llen;
	memcpy(info + n, th, 32);
	n += 32;
	info[n++] = 0x01;
	return sev_gpu_hmac_sha256(prk, 32, info, n, out);
}

/*
 * Shared key schedule (identical on both sides):
 *   transcript = pub_c || nonce_c || pub_s || nonce_s
 *   th         = SHA256(transcript)
 *   PRK        = HKDF-Extract(salt = PSK, IKM = Z)          [binds PSK + ECDH]
 *   comm_key   = HKDF-Expand(PRK, "sev-gpu comm v1"    || th)
 *   confirm_key= HKDF-Expand(PRK, "sev-gpu confirm v1" || th)
 */
int sev_gpu_hs_derive(const u8 *pub_c, const u8 *nonce_c,
			     const u8 *pub_s, const u8 *nonce_s,
			     const u8 *z, const u8 *psk,
			     u8 comm_key[32], u8 confirm_key[32], u8 th_out[32])
{
	u8 transcript[2 * (SEV_GPU_HS_PUBKEY_LEN + SEV_GPU_HS_NONCE_LEN)];
	u8 th[32], prk[32];
	int ret;

	memcpy(transcript, pub_c, SEV_GPU_HS_PUBKEY_LEN);
	memcpy(transcript + 64, nonce_c, SEV_GPU_HS_NONCE_LEN);
	memcpy(transcript + 96, pub_s, SEV_GPU_HS_PUBKEY_LEN);
	memcpy(transcript + 160, nonce_s, SEV_GPU_HS_NONCE_LEN);

	ret = sev_gpu_sha256(transcript, sizeof(transcript), th);
	if (ret)
		goto out;
	/* HKDF-Extract(salt=PSK, IKM=Z) */
	ret = sev_gpu_hmac_sha256(psk, SEV_GPU_COMM_KEY_LEN, z,
				  SEV_GPU_HS_SECRET_LEN, prk);
	if (ret)
		goto out;
	ret = sev_gpu_hkdf_expand32(prk, "sev-gpu comm v1", th, comm_key);
	if (ret)
		goto out;
	ret = sev_gpu_hkdf_expand32(prk, "sev-gpu confirm v1", th, confirm_key);
	if (ret)
		goto out;
	memcpy(th_out, th, 32);
out:
	memzero_explicit(prk, sizeof(prk));
	memzero_explicit(transcript, sizeof(transcript));
	return ret;
}

/* Ephemeral P-256 ECDH context (holds the private key inside the kpp tfm). */


/* Allocate an ephemeral keypair and export the public key into e->pub. */
int sev_gpu_ecdhe_init(struct sev_gpu_ecdhe *e)
{
	struct ecdh params = { .key = NULL, .key_size = 0 };
	struct kpp_request *req = NULL;
	struct scatterlist dst;
	DECLARE_CRYPTO_WAIT(wait);
	char *encoded = NULL;
	u8 *pubbuf = NULL;		/* heap: scatterlists must not sit on a
					 * VMAP_STACK stack buffer */
	unsigned int elen;
	int ret;

	e->tfm = crypto_alloc_kpp(SEV_GPU_HS_CURVE, 0, 0);
	if (IS_ERR(e->tfm)) {
		ret = PTR_ERR(e->tfm);
		e->tfm = NULL;
		return ret;
	}
	elen = crypto_ecdh_key_len(&params);
	encoded = kmalloc(elen, GFP_KERNEL);
	pubbuf = kmalloc(SEV_GPU_HS_PUBKEY_LEN, GFP_KERNEL);
	if (!encoded || !pubbuf) {
		ret = -ENOMEM;
		goto err;
	}
	/* key=NULL/key_size=0 -> kernel generates a random ephemeral privkey. */
	ret = crypto_ecdh_encode_key(encoded, elen, &params);
	if (ret)
		goto err;
	ret = crypto_kpp_set_secret(e->tfm, encoded, elen);
	if (ret)
		goto err;
	req = kpp_request_alloc(e->tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto err;
	}
	sg_init_one(&dst, pubbuf, SEV_GPU_HS_PUBKEY_LEN);
	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dst, SEV_GPU_HS_PUBKEY_LEN);
	kpp_request_set_callback(req,
				 CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				 crypto_req_done, &wait);
	ret = crypto_wait_req(crypto_kpp_generate_public_key(req), &wait);
	kpp_request_free(req);
	if (ret)
		goto err;
	memcpy(e->pub, pubbuf, SEV_GPU_HS_PUBKEY_LEN);
	kfree_sensitive(encoded);
	kfree_sensitive(pubbuf);
	return 0;
err:
	kfree_sensitive(encoded);
	kfree_sensitive(pubbuf);
	if (e->tfm) {
		crypto_free_kpp(e->tfm);
		e->tfm = NULL;
	}
	return ret;
}

/* Compute the ECDH shared secret with @peer_pub -> out[32]. */
int sev_gpu_ecdhe_shared(struct sev_gpu_ecdhe *e,
				const u8 peer_pub[SEV_GPU_HS_PUBKEY_LEN],
				u8 out[SEV_GPU_HS_SECRET_LEN])
{
	struct kpp_request *req;
	struct scatterlist src, dst;
	DECLARE_CRYPTO_WAIT(wait);
	u8 *inbuf, *outbuf;		/* heap: see sev_gpu_ecdhe_init */
	int ret;

	if (!e->tfm)
		return -EINVAL;
	inbuf = kmalloc(SEV_GPU_HS_PUBKEY_LEN, GFP_KERNEL);
	outbuf = kmalloc(SEV_GPU_HS_SECRET_LEN, GFP_KERNEL);
	if (!inbuf || !outbuf) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(inbuf, peer_pub, SEV_GPU_HS_PUBKEY_LEN);
	req = kpp_request_alloc(e->tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto out;
	}
	sg_init_one(&src, inbuf, SEV_GPU_HS_PUBKEY_LEN);
	sg_init_one(&dst, outbuf, SEV_GPU_HS_SECRET_LEN);
	kpp_request_set_input(req, &src, SEV_GPU_HS_PUBKEY_LEN);
	kpp_request_set_output(req, &dst, SEV_GPU_HS_SECRET_LEN);
	kpp_request_set_callback(req,
				 CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				 crypto_req_done, &wait);
	ret = crypto_wait_req(crypto_kpp_compute_shared_secret(req), &wait);
	kpp_request_free(req);
	if (!ret)
		memcpy(out, outbuf, SEV_GPU_HS_SECRET_LEN);
out:
	kfree_sensitive(inbuf);
	kfree_sensitive(outbuf);
	return ret;
}

void sev_gpu_ecdhe_free(struct sev_gpu_ecdhe *e)
{
	if (e->tfm) {
		crypto_free_kpp(e->tfm);
		e->tfm = NULL;
	}
	memzero_explicit(e->pub, sizeof(e->pub));
}

