/*
 * cc_crypt_selftest.c -- host-runnable known-answer test for the CC data-plane
 * crypto contract.
 *
 * This pins the exact wire format that the in-kernel SEV_GPU_IOC_CRYPT handler
 * (kernel/sev_gpu_manager.c) and the GPU CE must both agree on:
 *
 *   - 96-bit message counter, little-endian in the low 8 bytes, high 4 = 0
 *   - real GCM IV = counter XOR ivMask  (per byte, 12 bytes)
 *   - AES-256-GCM, no AAD, 16-byte tag
 *
 * It runs entirely in user space with OpenSSL -- no device node, no loaded
 * module, no GPU -- so it can gate CI and catch any drift in the IV derivation
 * or cipher parameters independently of the kernel build.
 *
 * Build:  gcc -Wall -Wextra -O2 cc_crypt_selftest.c -o cc_crypt_selftest -lssl -lcrypto
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>

#define KEY_LEN   32
#define IV_LEN    12
#define TAG_LEN   16

/* Mirror of the kernel CC IV derivation: gcm_iv = counterLE(ctr) XOR ivMask. */
static void cc_derive_iv(uint64_t ctr, const uint8_t iv_mask[IV_LEN],
			 uint8_t gcm_iv[IV_LEN])
{
	uint8_t counter[IV_LEN];
	int i;

	memset(counter, 0, IV_LEN);
	for (i = 0; i < 8; i++)
		counter[i] = (uint8_t)(ctr >> (8 * i));
	for (i = 0; i < IV_LEN; i++)
		gcm_iv[i] = counter[i] ^ iv_mask[i];
}

/* AES-256-GCM encrypt, no AAD. Returns 0 on success. */
static int gcm_encrypt(const uint8_t key[KEY_LEN], const uint8_t iv[IV_LEN],
		       const uint8_t *pt, int len, uint8_t *ct,
		       uint8_t tag[TAG_LEN])
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int outl = 0, tmp = 0, rc = -1;

	if (!ctx)
		return -1;
	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) != 1)
		goto out;
	if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1)
		goto out;
	if (EVP_EncryptUpdate(ctx, ct, &outl, pt, len) != 1)
		goto out;
	if (EVP_EncryptFinal_ex(ctx, ct + outl, &tmp) != 1)
		goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) != 1)
		goto out;
	rc = 0;
out:
	EVP_CIPHER_CTX_free(ctx);
	return rc;
}

/* AES-256-GCM decrypt, no AAD. Returns 0 on success, 1 on auth failure. */
static int gcm_decrypt(const uint8_t key[KEY_LEN], const uint8_t iv[IV_LEN],
		       const uint8_t *ct, int len, const uint8_t tag[TAG_LEN],
		       uint8_t *pt)
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int outl = 0, tmp = 0, rc = -1;

	if (!ctx)
		return -1;
	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) != 1)
		goto out;
	if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1)
		goto out;
	if (EVP_DecryptUpdate(ctx, pt, &outl, ct, len) != 1)
		goto out;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN,
				(void *)tag) != 1)
		goto out;
	rc = (EVP_DecryptFinal_ex(ctx, pt + outl, &tmp) == 1) ? 0 : 1;
out:
	EVP_CIPHER_CTX_free(ctx);
	return rc;
}

static void hexdump(const char *label, const uint8_t *p, int n)
{
	int i;

	printf("%s", label);
	for (i = 0; i < n; i++)
		printf("%02x", p[i]);
	printf("\n");
}

static int eq(const char *what, const uint8_t *a, const uint8_t *b, int n)
{
	if (memcmp(a, b, n) == 0) {
		printf("[+] %s matches expected\n", what);
		return 0;
	}
	printf("[-] %s MISMATCH\n", what);
	hexdump("    got: ", a, n);
	hexdump("    exp: ", b, n);
	return 1;
}

int main(void)
{
	/* Fixed known-answer vector (arbitrary but pinned). */
	static const uint8_t key[KEY_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	};
	static const uint8_t iv_mask[IV_LEN] = {
		0xa5, 0x5a, 0xff, 0x00, 0x11, 0x22,
		0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	};
	const uint64_t ctr = 1;
	uint8_t pt[64], ct[64], rt[64], tag[TAG_LEN];
	uint8_t gcm_iv[IV_LEN];
	int i, fails = 0;

	/* Expected derived IV for ctr=1: counterLE = 01 00..00, XOR ivMask. */
	static const uint8_t exp_iv[IV_LEN] = {
		0xa4, 0x5a, 0xff, 0x00, 0x11, 0x22,
		0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	};
	/* Known-answer ciphertext + tag (AES-256-GCM, no AAD) for the vector
	 * above over the 64-byte plaintext pt[i] = i*7+3. */
	static const uint8_t exp_ct[64] = {
		0x8b, 0x1d, 0x9b, 0xef, 0x63, 0xcf, 0x2f, 0x83,
		0x4c, 0x15, 0x48, 0x62, 0x61, 0xf7, 0x9b, 0x5f,
		0x7b, 0x65, 0x68, 0xa6, 0x1c, 0x93, 0x38, 0x0a,
		0x82, 0x52, 0xa6, 0xe6, 0xf6, 0x98, 0x9f, 0x05,
		0xc6, 0x19, 0x46, 0x52, 0x09, 0xfd, 0x4f, 0x4a,
		0x88, 0xfa, 0xca, 0x49, 0xf9, 0xa2, 0x2c, 0xc9,
		0xa6, 0x7c, 0x6f, 0x44, 0x7d, 0xf6, 0x6a, 0xde,
		0xba, 0x2f, 0xef, 0x2c, 0x53, 0xec, 0xe0, 0x65,
	};
	static const uint8_t exp_tag[TAG_LEN] = {
		0xd9, 0xca, 0x6a, 0x89, 0xb1, 0x0d, 0x48, 0x8c,
		0xcf, 0x85, 0xbc, 0x25, 0xf6, 0xc4, 0x2c, 0x58,
	};

	for (i = 0; i < (int)sizeof(pt); i++)
		pt[i] = (uint8_t)(i * 7 + 3);

	printf("=== CC data-plane crypto known-answer self-test (host) ===\n");

	/* 1. IV derivation contract. */
	cc_derive_iv(ctr, iv_mask, gcm_iv);
	fails += eq("derived GCM IV (counterLE ^ ivMask)", gcm_iv, exp_iv, IV_LEN);

	/* 2. Encrypt and emit the produced vector (for baking the KAT). */
	if (gcm_encrypt(key, gcm_iv, pt, sizeof(pt), ct, tag) != 0) {
		printf("[-] encrypt failed\n");
		return 1;
	}
	hexdump("[*] ciphertext: ", ct, sizeof(ct));
	hexdump("[*] tag       : ", tag, TAG_LEN);
	fails += eq("ciphertext (known-answer)", ct, exp_ct, sizeof(ct));
	fails += eq("tag (known-answer)", tag, exp_tag, TAG_LEN);

	/* 3. Round trip: decrypt recovers the plaintext. */
	if (gcm_decrypt(key, gcm_iv, ct, sizeof(ct), tag, rt) != 0) {
		printf("[-] decrypt/auth failed on good ciphertext\n");
		fails++;
	} else if (memcmp(rt, pt, sizeof(pt)) != 0) {
		printf("[-] round-trip plaintext mismatch\n");
		fails++;
	} else {
		printf("[+] encrypt->decrypt round trip recovered plaintext\n");
	}

	/* 4. Negative: a tampered tag must fail authentication. */
	tag[0] ^= 0x01;
	if (gcm_decrypt(key, gcm_iv, ct, sizeof(ct), tag, rt) == 0) {
		printf("[-] tampered tag was ACCEPTED (auth broken)\n");
		fails++;
	} else {
		printf("[+] tampered tag correctly rejected\n");
	}
	tag[0] ^= 0x01;

	/* 5. Negative: wrong IV (counter reuse drift) must fail. */
	gcm_iv[0] ^= 0x01;
	if (gcm_decrypt(key, gcm_iv, ct, sizeof(ct), tag, rt) == 0) {
		printf("[-] wrong IV was ACCEPTED (IV not bound)\n");
		fails++;
	} else {
		printf("[+] wrong IV correctly rejected\n");
	}

	if (fails == 0)
		printf("\nRESULT: PASS -- CC crypto contract verified\n");
	else
		printf("\nRESULT: FAIL -- %d check(s) failed\n", fails);
	return fails ? 1 : 0;
}
