// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_kmb.c — KMB seal/install/send/recv (moved from sev_gpu_manager.c).
 * Depends on: crypto (aead, kmb_fp), state (comm_keystore), and the transport
 * mailbox accessor kmb_mailbox() (defined in sev_gpu_main.c, touches ctrl_dev).
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_crypto.h"
#include "sev_gpu_state.h"
#include "sev_gpu_kmb.h"

int sev_gpu_send_kmb(u8 vm_id, u32 channel_id, unsigned int to_ms,
			    u8 fp_out[8])
{
	struct sev_gpu_kmb_aad aad;
	struct sev_cc_kmb kmb;
	struct sev_gpu_assignment *slot = NULL;
	void __iomem *mb;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce[SEV_GPU_KMB_NONCE_LEN];
	u8 tag[SEV_GPU_KMB_TAG_LEN];
	u8 fp[8];
	u32 keyspace = 0, seq;
	unsigned long deadline;
	bool have_key;
	int i, ret;

	if (vm_id >= SEV_GPU_MAX_VMS)
		return -EINVAL;

	mb = kmb_mailbox(vm_id);
	if (!mb)
		return -ENXIO;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm_id, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[vm_id], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key)
		return -ENOKEY;

	spin_lock(&assign_state.lock);
	for (i = 0; i < SEV_GPU_MAX_CHANNELS_PER_VM; i++) {
		struct sev_gpu_assignment *e = &assign_state.a[vm_id][i];

		if (e->in_use && e->channel_id == channel_id) {
			kmb = e->kmb;
			keyspace = e->keyspace;
			slot = e;
			break;
		}
	}
	spin_unlock(&assign_state.lock);
	if (!slot) {
		memzero_explicit(key, sizeof(key));
		return -ENOENT;	/* channel not assigned to this client */
	}

	ret = sev_gpu_kmb_fp(&kmb, fp);
	if (ret)
		goto send_out;

	get_random_bytes(nonce, sizeof(nonce));
	seq = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, seq)) + 1;

	/* Record this delivery's seq as the channel's KMB epoch (generation). */
	spin_lock(&assign_state.lock);
	slot->generation = seq;
	spin_unlock(&assign_state.lock);

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = vm_id;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Seal the KMB in place; kmb now holds ciphertext. */
	ret = sev_gpu_aead(true, key, nonce, &aad, sizeof(aad),
			   &kmb, sizeof(kmb), tag);
	if (ret)
		goto send_out;

	/* Publish ciphertext + metadata, then flip the slot to READY. */
	iowrite32(SEV_GPU_KMB_IDLE,
		  mb + offsetof(struct sev_gpu_kmb_mbox, state));
	iowrite32(SEV_GPU_KMB_MAGIC,
		  mb + offsetof(struct sev_gpu_kmb_mbox, magic));
	iowrite32(vm_id, mb + offsetof(struct sev_gpu_kmb_mbox, vm_id));
	iowrite32(channel_id,
		  mb + offsetof(struct sev_gpu_kmb_mbox, channel_id));
	iowrite32(keyspace,
		  mb + offsetof(struct sev_gpu_kmb_mbox, keyspace));
	iowrite32(seq, mb + offsetof(struct sev_gpu_kmb_mbox, seq));
	iowrite32((u32)sizeof(kmb),
		  mb + offsetof(struct sev_gpu_kmb_mbox, ct_len));
	memcpy_toio(mb + offsetof(struct sev_gpu_kmb_mbox, nonce),
		    nonce, sizeof(nonce));
	memcpy_toio(mb + offsetof(struct sev_gpu_kmb_mbox, tag),
		    tag, sizeof(tag));
	memcpy_toio(mb + offsetof(struct sev_gpu_kmb_mbox, ct),
		    &kmb, sizeof(kmb));
	wmb();
	iowrite32(SEV_GPU_KMB_READY,
		  mb + offsetof(struct sev_gpu_kmb_mbox, state));

	/* Wait for the client to consume + install it. */
	to_ms = to_ms ? to_ms : 120000;
	deadline = jiffies + msecs_to_jiffies(to_ms);
	ret = -ETIMEDOUT;
	while (time_before(jiffies, deadline)) {
		if (ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, state)) ==
		    SEV_GPU_KMB_ACK) {
			ret = 0;
			break;
		}
		if (msleep_interruptible(20)) {
			ret = -EINTR;
			break;
		}
	}

	if (ret == 0 && fp_out)
		memcpy(fp_out, fp, 8);

send_out:
	memzero_explicit(&kmb, sizeof(kmb));
	memzero_explicit(key, sizeof(key));
	return ret;
}

/*
 * Client: block until the manager posts a sealed KMB (up to to_ms, 0 = default
 * 120s), unseal it under the comm key, and install it into the per-channel
 * keystore. Fills out_channel_id/out_keyspace/fp_out[8] (when non-NULL).
 */
int sev_gpu_recv_kmb(struct sev_gpu_dev *d, unsigned int to_ms,
			    u32 *out_channel_id, u32 *out_keyspace, u8 fp_out[8])
{
	struct sev_gpu_kmb_aad aad;
	struct sev_cc_kmb kmb;
	void __iomem *mb;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce[SEV_GPU_KMB_NONCE_LEN];
	u8 tag[SEV_GPU_KMB_TAG_LEN];
	u8 fp[8];
	u32 ct_len, channel_id, keyspace, seq, vm = d->comm_vm_id;
	unsigned long deadline;
	bool have_key;
	int ret;

	mb = kmb_mailbox(vm);
	if (!mb)
		return -ENXIO;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[vm], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key)
		return -ENOKEY;

	/* Wait for the manager to post a sealed KMB. */
	to_ms = to_ms ? to_ms : 120000;
	deadline = jiffies + msecs_to_jiffies(to_ms);
	ret = -ETIMEDOUT;
	while (time_before(jiffies, deadline)) {
		if (ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, state)) ==
		    SEV_GPU_KMB_READY) {
			ret = 0;
			break;
		}
		if (msleep_interruptible(20)) {
			ret = -EINTR;
			break;
		}
	}
	if (ret) {
		memzero_explicit(key, sizeof(key));
		return ret;
	}

	rmb();
	ct_len     = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, ct_len));
	channel_id = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, channel_id));
	keyspace   = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, keyspace));
	seq        = ioread32(mb + offsetof(struct sev_gpu_kmb_mbox, seq));
	if (ct_len != sizeof(kmb)) {
		ret = -EPROTO;
		goto recv_out;
	}
	memcpy_fromio(nonce, mb + offsetof(struct sev_gpu_kmb_mbox, nonce),
		      sizeof(nonce));
	memcpy_fromio(tag, mb + offsetof(struct sev_gpu_kmb_mbox, tag),
		      sizeof(tag));
	memcpy_fromio(&kmb, mb + offsetof(struct sev_gpu_kmb_mbox, ct),
		      sizeof(kmb));

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = vm;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Unseal in place; auth failure (tampered ciphertext) => -EBADMSG. */
	ret = sev_gpu_aead(false, key, nonce, &aad, sizeof(aad),
			   &kmb, sizeof(kmb), tag);
	if (ret)
		goto recv_out;

	ret = sev_gpu_kmb_fp(&kmb, fp);
	if (ret)
		goto recv_out;

	/* Install the unsealed KMB into the client's per-channel keystore. */
	{
		int slot = -1, j;

		spin_lock(&client_kmb_store.lock);
		for (j = 0; j < SEV_GPU_MAX_CHANNELS_PER_VM; j++) {
			struct sev_client_chan *e = &client_kmb_store.c[j];

			if (e->valid && e->channel_id == channel_id) {
				slot = j;
				break;
			}
			if (slot < 0 && !e->valid)
				slot = j;
		}
		if (slot >= 0) {
			struct sev_client_chan *e = &client_kmb_store.c[slot];

			e->kmb        = kmb;
			e->channel_id = channel_id;
			e->keyspace   = keyspace;
			e->generation = seq;	/* KMB epoch */
			e->ctr_h2d    = 0;	/* fresh key => reset IV */
			e->ctr_d2h    = 0;
			e->valid      = true;
		}
		spin_unlock(&client_kmb_store.lock);
		if (slot < 0) {
			ret = -ENOSPC;	/* no free channel slot */
			goto recv_out;
		}
	}

	if (out_channel_id)
		*out_channel_id = channel_id;
	if (out_keyspace)
		*out_keyspace = keyspace;
	if (fp_out)
		memcpy(fp_out, fp, 8);

	/* Ack so the manager unblocks. */
	wmb();
	iowrite32(SEV_GPU_KMB_ACK,
		  mb + offsetof(struct sev_gpu_kmb_mbox, state));

	pr_info("sev_gpu: recv KMB ch %u fp %02x%02x%02x%02x%02x%02x%02x%02x\n",
		channel_id, fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6],
		fp[7]);

recv_out:
	memzero_explicit(&kmb, sizeof(kmb));
	memzero_explicit(key, sizeof(key));
	return ret;
}
u32 sev_gpu_kmb_seal_impl(u32 client_id, u32 channel_id,
				 const void *kmb_plain, u32 kmb_len,
				 void *out_nonce, void *out_tag, void *out_ct,
				 u32 *out_seq, u32 *out_keyspace)
{
	struct sev_gpu_kmb_aad aad;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce[SEV_GPU_KMB_NONCE_LEN];
	u8 tag[SEV_GPU_KMB_TAG_LEN];
	u8 ct[sizeof(struct sev_cc_kmb)];
	u32 seq, keyspace = 0;
	bool have_key;
	int ret;

	if (!kmb_plain || !out_nonce || !out_tag || !out_ct || !out_seq ||
	    !out_keyspace)
		return SEV_KMB_NV_ERR;
	if (kmb_len != sizeof(struct sev_cc_kmb) || client_id >= SEV_GPU_MAX_VMS)
		return SEV_KMB_NV_ERR;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(client_id, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[client_id], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key && auto_mtls &&
	    sev_gpu_wait_comm_key(client_id, auto_mtls_wait_ms)) {
		spin_lock(&comm_keystore.lock);
		have_key = test_bit(client_id, &comm_keystore.valid);
		if (have_key)
			memcpy(key, comm_keystore.key[client_id],
			       SEV_GPU_COMM_KEY_LEN);
		spin_unlock(&comm_keystore.lock);
	}
	if (!have_key) {
		pr_warn("sev_gpu: GET_KMB seal: no comm key for vm %u\n", client_id);
		return SEV_KMB_NV_ERR;
	}

	seq = (u32)atomic_inc_return(&sev_gpu_kmb_pull_seq);
	get_random_bytes(nonce, sizeof(nonce));
	memcpy(ct, kmb_plain, kmb_len);

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = client_id;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Seal in place: ct now holds ciphertext, tag holds the GCM tag. */
	ret = sev_gpu_aead(true, key, nonce, &aad, sizeof(aad), ct, kmb_len, tag);
	memzero_explicit(key, sizeof(key));
	if (ret) {
		memzero_explicit(ct, sizeof(ct));
		pr_warn("sev_gpu: GET_KMB seal: aead failed %d\n", ret);
		return SEV_KMB_NV_ERR;
	}

	memcpy(out_nonce, nonce, sizeof(nonce));
	memcpy(out_tag, tag, sizeof(tag));
	memcpy(out_ct, ct, kmb_len);
	*out_seq = seq;
	*out_keyspace = keyspace;
	memzero_explicit(ct, sizeof(ct));

	pr_info("sev_gpu: GET_KMB sealed ch 0x%x for vm %u seq %u\n",
		channel_id, client_id, seq);
	return SEV_KMB_NV_OK;
}

/*
 * Client: unseal a sealed CC_KMB delivered in a GET_KMB reply and install it in
 * the per-channel keystore (consumed later by the data-plane crypto). On success
 * the plaintext CC_KMB is written to kmb_out (a kernel buffer the caller copies
 * to CUDA's params). Authentication failure (tampered/mis-keyed ciphertext)
 * returns an error and installs nothing.
 */
u32 sev_gpu_kmb_install_impl(u32 channel_id, u32 seq, u32 keyspace,
				    const void *nonce, const void *tag,
				    const void *ct, u32 ct_len, void *kmb_out)
{
	struct sev_gpu_dev *d = ctrl_dev;
	struct sev_gpu_kmb_aad aad;
	struct sev_cc_kmb kmb;
	u8 key[SEV_GPU_COMM_KEY_LEN];
	u8 nonce_b[SEV_GPU_KMB_NONCE_LEN];
	u8 tag_b[SEV_GPU_KMB_TAG_LEN];
	u32 vm;
	bool have_key;
	int ret, slot = -1, j;

	if (!d || d->is_manager)
		return SEV_KMB_NV_ERR;
	if (!nonce || !tag || !ct || !kmb_out || ct_len != sizeof(kmb))
		return SEV_KMB_NV_ERR;

	vm = d->comm_vm_id;

	/*
	 * If the in-kernel handshake has not yet delivered the comm key, drive
	 * it now (keyed by our slot index) and re-read the negotiated vm.
	 */
	if (auto_mtls) {
		bool ready = false;

		spin_lock(&comm_keystore.lock);
		if (vm < SEV_GPU_MAX_VMS)
			ready = test_bit(vm, &comm_keystore.valid);
		spin_unlock(&comm_keystore.lock);
		if (!ready) {
			sev_gpu_hs_client_maybe_run(d->client_vm_id);
			vm = d->comm_vm_id;
		}
	}
	if (vm >= SEV_GPU_MAX_VMS)
		return SEV_KMB_NV_ERR;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm, &comm_keystore.valid);
	if (have_key)
		memcpy(key, comm_keystore.key[vm], SEV_GPU_COMM_KEY_LEN);
	spin_unlock(&comm_keystore.lock);
	if (!have_key && auto_mtls &&
	    sev_gpu_wait_comm_key(vm, auto_mtls_wait_ms)) {
		spin_lock(&comm_keystore.lock);
		have_key = test_bit(vm, &comm_keystore.valid);
		if (have_key)
			memcpy(key, comm_keystore.key[vm], SEV_GPU_COMM_KEY_LEN);
		spin_unlock(&comm_keystore.lock);
	}
	if (!have_key) {
		pr_warn("sev_gpu: GET_KMB install: no comm key for vm %u\n", vm);
		return SEV_KMB_NV_ERR;
	}

	memcpy(&kmb, ct, ct_len);
	memcpy(nonce_b, nonce, sizeof(nonce_b));
	memcpy(tag_b, tag, sizeof(tag_b));

	aad.magic      = SEV_GPU_KMB_MAGIC;
	aad.vm_id      = vm;
	aad.channel_id = channel_id;
	aad.keyspace   = keyspace;
	aad.seq        = seq;

	/* Unseal in place; auth failure => nonzero ret, kmb is meaningless. */
	ret = sev_gpu_aead(false, key, nonce_b, &aad, sizeof(aad),
			   &kmb, ct_len, tag_b);
	memzero_explicit(key, sizeof(key));
	if (ret) {
		memzero_explicit(&kmb, sizeof(kmb));
		pr_warn("sev_gpu: GET_KMB install: unseal failed %d (auth?)\n", ret);
		return SEV_KMB_NV_ERR;
	}

	/* Install into the client's per-channel keystore (reuse or first free). */
	spin_lock(&client_kmb_store.lock);
	for (j = 0; j < SEV_GPU_MAX_CHANNELS_PER_VM; j++) {
		struct sev_client_chan *e = &client_kmb_store.c[j];

		if (e->valid && e->channel_id == channel_id) {
			slot = j;
			break;
		}
		if (slot < 0 && !e->valid)
			slot = j;
	}
	if (slot >= 0) {
		struct sev_client_chan *e = &client_kmb_store.c[slot];

		e->kmb        = kmb;
		e->channel_id = channel_id;
		e->keyspace   = keyspace;
		e->generation = seq;	/* KMB epoch */
		e->ctr_h2d    = 0;	/* fresh key => reset IV counters */
		e->ctr_d2h    = 0;
		e->valid      = true;
	}
	spin_unlock(&client_kmb_store.lock);
	if (slot < 0) {
		memzero_explicit(&kmb, sizeof(kmb));
		pr_warn("sev_gpu: GET_KMB install: keystore full (ch 0x%x)\n",
			channel_id);
		return SEV_KMB_NV_ERR;
	}

	memcpy(kmb_out, &kmb, ct_len);
	memzero_explicit(&kmb, sizeof(kmb));

	pr_info("sev_gpu: GET_KMB installed ch 0x%x vm %u seq %u\n",
		channel_id, vm, seq);
	return SEV_KMB_NV_OK;
}