// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_handshake.c — ECDHE-PSK key agreement + comm-key commit.
 * Moved from sev_gpu_manager.c. Depends on crypto, state, and the transport
 * accessor hs_ctrl_mailbox() (defined in sev_gpu_main.c).
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_crypto.h"
#include "sev_gpu_state.h"
#include "sev_gpu_kmb.h"
#include "sev_gpu_handshake.h"
#include <crypto/utils.h>  /* crypto_memneq */

/* Bounded wait for the comm key of @vm to appear (handshake completing on
 * another thread / the peer). Kept below RPC_TIMEOUT_MS by the caller. */
/* Commit an established comm key into comm_keystore (shared by SET_COMM_KEY
 * and the in-kernel handshake). */
void sev_gpu_commit_comm_key(u32 vm, const u8 key[SEV_GPU_COMM_KEY_LEN])
{
	spin_lock(&comm_keystore.lock);
	memcpy(comm_keystore.key[vm], key, SEV_GPU_COMM_KEY_LEN);
	set_bit(vm, &comm_keystore.valid);
	spin_unlock(&comm_keystore.lock);

	/* Comm channel + KMB now established for this client: this is the gate for
	 * building the manager's per-client UVM channels (queued to a workqueue, so
	 * the heavy retain never runs under comm_keystore.lock or on the poller). */
	sev_gpu_manager_note_client_active(vm);
}

bool sev_gpu_wait_comm_key(u32 vm, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	if (vm >= SEV_GPU_MAX_VMS)
		return false;
	for (;;) {
		bool ok;

		spin_lock(&comm_keystore.lock);
		ok = test_bit(vm, &comm_keystore.valid);
		spin_unlock(&comm_keystore.lock);
		if (ok)
			return true;
		if (time_after(jiffies, deadline) || signal_pending(current))
			return false;
		msleep(20);
	}
}

/*
 * Manager: service one handshake mailbox found in the REQ state. Runs in the
 * single manager poller kthread context (see rpc_thread_fn). HELLO derives the
 * shared secret + keys and returns pub_s/nonce_s/confirm_s; FINISHED verifies
 * the client's confirm and, on success, commits the comm key.
 */
void sev_gpu_hs_service_slot(u8 vm, void __iomem *hb)
{
	sev_gpu_hs_slot_t s;
	struct sev_gpu_ecdhe e = { .tfm = NULL };
	u8 psk[SEV_GPU_COMM_KEY_LEN];
	u8 z[SEV_GPU_HS_SECRET_LEN];
	u8 comm_key[32], confirm_key[32], th[32], mac[32];
	u32 status = 1;
	int ret;

	memcpy_fromio(&s, hb, sizeof(s));
	if (s.magic != SEV_GPU_HS_MAGIC || s.version != SEV_GPU_HS_VERSION) {
		/* stale/garbage: drop back to idle so we don't spin on it */
		iowrite32(SEV_GPU_HS_IDLE,
			  hb + offsetof(sev_gpu_hs_slot_t, state));
		return;
	}
	if (sev_gpu_get_psk(psk))
		goto reply;

	if (s.msg_type == SEV_GPU_HS_MSG_HELLO) {
		ret = sev_gpu_ecdhe_init(&e);
		if (ret) {
			pr_warn("sev_gpu: auto-mTLS: mgr ecdhe init %d\n", ret);
			goto reply;
		}
		memcpy(s.pub_s, e.pub, sizeof(s.pub_s));
		get_random_bytes(s.nonce_s, sizeof(s.nonce_s));
		ret = sev_gpu_ecdhe_shared(&e, s.pub_c, z);
		if (ret) {
			pr_warn("sev_gpu: auto-mTLS: mgr ecdh shared %d\n", ret);
			goto reply;
		}
		ret = sev_gpu_hs_derive(s.pub_c, s.nonce_c, s.pub_s, s.nonce_s,
					z, psk, comm_key, confirm_key, th);
		if (ret)
			goto reply;
		ret = sev_gpu_hmac_sha256(confirm_key, sizeof(confirm_key),
					  (const u8 *)"s", 1, mac);
		if (ret)
			goto reply;
		memcpy(s.confirm_s, mac, sizeof(s.confirm_s));
		memcpy(hs_mgr_state[vm].comm_key, comm_key, sizeof(comm_key));
		memcpy(hs_mgr_state[vm].confirm_key, confirm_key,
		       sizeof(confirm_key));
		memcpy(hs_mgr_state[vm].th, th, sizeof(th));
		hs_mgr_state[vm].active = true;
		status = 0;
		pr_info("sev_gpu: auto-mTLS: mgr HELLO ok for vm %u\n", vm);
	} else if (s.msg_type == SEV_GPU_HS_MSG_FINISHED) {
		if (!hs_mgr_state[vm].active) {
			pr_warn("sev_gpu: auto-mTLS: FINISHED without HELLO vm %u\n",
				vm);
			goto reply;
		}
		ret = sev_gpu_hmac_sha256(hs_mgr_state[vm].confirm_key,
					  SEV_GPU_HS_CONFIRM_LEN,
					  (const u8 *)"c", 1, mac);
		if (ret)
			goto clear;
		if (crypto_memneq(mac, s.confirm_c, sizeof(mac))) {
			pr_warn("sev_gpu: auto-mTLS: client confirm mismatch vm %u (MITM/PSK?)\n",
				vm);
			goto clear;
		}
		sev_gpu_commit_comm_key(vm, hs_mgr_state[vm].comm_key);
		status = 0;
		pr_info("sev_gpu: auto-mTLS: mgr FINISHED ok, comm key committed for vm %u\n",
			vm);
clear:
		memzero_explicit(&hs_mgr_state[vm], sizeof(hs_mgr_state[vm]));
	} else {
		pr_warn("sev_gpu: auto-mTLS: unknown msg_type %u vm %u\n",
			s.msg_type, vm);
	}

reply:
	s.status = status;
	s.state = SEV_GPU_HS_IDLE;		/* publish payload before state */
	memcpy_toio(hb, &s, sizeof(s));
	wmb();
	iowrite32(SEV_GPU_HS_REPLY, hb + offsetof(sev_gpu_hs_slot_t, state));
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, (u16)vm, IVSHMEM_VECTOR_RPC);

	sev_gpu_ecdhe_free(&e);
	memzero_explicit(psk, sizeof(psk));
	memzero_explicit(z, sizeof(z));
	memzero_explicit(comm_key, sizeof(comm_key));
	memzero_explicit(confirm_key, sizeof(confirm_key));
}

/* Client: poll the handshake mailbox until it flips to REPLY (bounded). */
static int sev_gpu_hs_wait_reply(void __iomem *hb)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(auto_mtls_wait_ms);

	for (;;) {
		if (ioread32(hb + offsetof(sev_gpu_hs_slot_t, state)) ==
		    SEV_GPU_HS_REPLY)
			return 0;
		if (time_after(jiffies, deadline) || signal_pending(current))
			return -ETIMEDOUT;
		usleep_range(200, 500);
	}
}

/*
 * Client: drive the full ECDHE-PSK handshake for @vm inline (blocking, ~2 RTT
 * over shared memory). On success installs the derived comm key and returns 0.
 */
int sev_gpu_hs_client_run(u32 vm)
{
	void __iomem *hb = hs_ctrl_mailbox((u8)vm);
	struct sev_gpu_ecdhe e = { .tfm = NULL };
	sev_gpu_hs_slot_t s;
	u8 psk[SEV_GPU_COMM_KEY_LEN];
	u8 z[SEV_GPU_HS_SECRET_LEN];
	u8 my_nonce_c[SEV_GPU_HS_NONCE_LEN];
	u8 comm_key[32], confirm_key[32], th[32], mac[32];
	int ret;

	if (!hb)
		return -ENODEV;
	ret = sev_gpu_get_psk(psk);
	if (ret)
		return ret;
	ret = sev_gpu_ecdhe_init(&e);
	if (ret) {
		memzero_explicit(psk, sizeof(psk));
		return ret;
	}

	/* ---- Phase 1: ClientHello {pub_c, nonce_c} ---- */
	get_random_bytes(my_nonce_c, sizeof(my_nonce_c));
	memset(&s, 0, sizeof(s));
	s.magic = SEV_GPU_HS_MAGIC;
	s.version = SEV_GPU_HS_VERSION;
	s.msg_type = SEV_GPU_HS_MSG_HELLO;
	memcpy(s.pub_c, e.pub, sizeof(s.pub_c));
	memcpy(s.nonce_c, my_nonce_c, sizeof(s.nonce_c));
	s.state = SEV_GPU_HS_IDLE;
	memcpy_toio(hb, &s, sizeof(s));
	wmb();
	iowrite32(SEV_GPU_HS_REQ, hb + offsetof(sev_gpu_hs_slot_t, state));
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, sev_gpu_manager_peer(ctrl_dev),
			     IVSHMEM_VECTOR_RPC);

	ret = sev_gpu_hs_wait_reply(hb);
	if (ret)
		goto out;
	memcpy_fromio(&s, hb, sizeof(s));
	if (s.status != 0) {
		pr_warn("sev_gpu: auto-mTLS: mgr rejected HELLO (status %u) vm %u\n",
			s.status, vm);
		ret = -EPROTO;
		goto out;
	}

	/* Derive using our own local pub_c/nonce_c (tamper -> key divergence). */
	ret = sev_gpu_ecdhe_shared(&e, s.pub_s, z);
	if (ret)
		goto out;
	ret = sev_gpu_hs_derive(e.pub, my_nonce_c, s.pub_s, s.nonce_s,
				z, psk, comm_key, confirm_key, th);
	if (ret)
		goto out;
	ret = sev_gpu_hmac_sha256(confirm_key, sizeof(confirm_key),
				  (const u8 *)"s", 1, mac);
	if (ret)
		goto out;
	if (crypto_memneq(mac, s.confirm_s, sizeof(mac))) {
		pr_warn("sev_gpu: auto-mTLS: server confirm mismatch vm %u (MITM/PSK?)\n",
			vm);
		ret = -EKEYREJECTED;
		goto out;
	}

	/* ---- Phase 2: Finished {confirm_c} ---- */
	ret = sev_gpu_hmac_sha256(confirm_key, sizeof(confirm_key),
				  (const u8 *)"c", 1, mac);
	if (ret)
		goto out;
	memset(&s, 0, sizeof(s));
	s.magic = SEV_GPU_HS_MAGIC;
	s.version = SEV_GPU_HS_VERSION;
	s.msg_type = SEV_GPU_HS_MSG_FINISHED;
	memcpy(s.confirm_c, mac, sizeof(s.confirm_c));
	s.state = SEV_GPU_HS_IDLE;
	memcpy_toio(hb, &s, sizeof(s));
	wmb();
	iowrite32(SEV_GPU_HS_REQ, hb + offsetof(sev_gpu_hs_slot_t, state));
	if (ctrl_dev)
		ivshmem_ring(ctrl_dev, sev_gpu_manager_peer(ctrl_dev),
			     IVSHMEM_VECTOR_RPC);

	ret = sev_gpu_hs_wait_reply(hb);
	if (ret)
		goto out;
	memcpy_fromio(&s, hb, sizeof(s));
	if (s.status != 0) {
		pr_warn("sev_gpu: auto-mTLS: mgr rejected FINISHED (status %u) vm %u\n",
			s.status, vm);
		ret = -EPROTO;
		goto out;
	}

	/* Both sides confirmed: install the comm key. */
	if (ctrl_dev && !ctrl_dev->is_manager)
		ctrl_dev->comm_vm_id = (u8)vm;
	sev_gpu_commit_comm_key(vm, comm_key);
	pr_info("sev_gpu: auto-mTLS: client handshake complete, comm key installed for vm %u\n",
		vm);
	ret = 0;
out:
	sev_gpu_ecdhe_free(&e);
	memzero_explicit(psk, sizeof(psk));
	memzero_explicit(z, sizeof(z));
	memzero_explicit(comm_key, sizeof(comm_key));
	memzero_explicit(confirm_key, sizeof(confirm_key));
	return ret;
}

/*
 * Client: opportunistically run the in-kernel handshake for @vm on first
 * contact, if enabled and no comm key exists yet. One runner at a time; other
 * threads proceed (their sealed-KMB path waits for the key via
 * sev_gpu_wait_comm_key). Bounded retries guard against a not-yet-ready peer.
 */
void sev_gpu_hs_client_maybe_run(u8 vm)
{
	bool have_key;
	int ret;

	if (!auto_mtls || vm >= SEV_GPU_MAX_VMS)
		return;

	spin_lock(&comm_keystore.lock);
	have_key = test_bit(vm, &comm_keystore.valid);
	spin_unlock(&comm_keystore.lock);
	if (have_key)
		return;
	if (atomic_read(&hs_client_attempts[vm]) >= SEV_GPU_HS_MAX_ATTEMPTS)
		return;
	if (atomic_cmpxchg(&hs_client_busy[vm], 0, 1) != 0)
		return;

	atomic_inc(&hs_client_attempts[vm]);
	ret = sev_gpu_hs_client_run(vm);
	atomic_set(&hs_client_busy[vm], 0);
	if (ret)
		pr_warn_ratelimited("sev_gpu: auto-mTLS: client handshake attempt failed (%d) for vm %u\n",
				    ret, vm);
}

/*
 * Manager: seal a freshly-fetched channel CC_KMB under the requesting client's
 * comm key. Fills the caller-provided nonce/tag/ciphertext buffers and reports
 * the AAD-binding seq + keyspace. The plaintext KMB (kmb_plain) is the manager's
 * private, SEV-encrypted memory; only the sealed ciphertext ever crosses the
 * host-visible ivshmem region.
 */

/*
 * Automatic KMB-handshake worker. Driven from SET_COMM_KEY: the manager assigns
 * a pre-provisioned channel of hs_keyspace to each pending client and seals its
 * KMB; the client receives + installs the KMB its manager posted. All the
 * blocking waits happen here, off the ioctl path.
 */
void sev_gpu_hs_work(struct work_struct *w)
{
	struct sev_gpu_dev *d = ctrl_dev;
	unsigned int to_ms = hs_timeout_ms;

	if (!d)
		return;

	if (d->is_manager) {
		unsigned long pend;
		unsigned int vm;

		spin_lock(&hs_state.lock);
		pend = hs_state.pending;
		hs_state.pending = 0;
		spin_unlock(&hs_state.lock);

		for_each_set_bit(vm, &pend, SEV_GPU_MAX_VMS) {
			u32 channel_id = 0;
			u8 fp[8];
			int rc;

			rc = sev_gpu_assign_channel((u8)vm, hs_keyspace, 0, 0, 0,
						    &channel_id, NULL, NULL);
			if (rc) {
				pr_warn("sev_gpu: auto-handshake VM%u assign failed (%d) -- provision keyspace %u first?\n",
					vm, rc, hs_keyspace);
				continue;
			}
			rc = sev_gpu_send_kmb((u8)vm, channel_id, to_ms, fp);
			if (rc)
				pr_warn("sev_gpu: auto-handshake VM%u send KMB ch %u failed (%d)\n",
					vm, channel_id, rc);
			else
				pr_info("sev_gpu: auto-handshake VM%u channel %u KMB delivered (fp %02x%02x%02x%02x)\n",
					vm, channel_id, fp[0], fp[1], fp[2],
					fp[3]);
		}
	} else {
		u32 channel_id = 0, keyspace = 0;
		u8 fp[8];
		int rc = sev_gpu_recv_kmb(d, to_ms, &channel_id, &keyspace, fp);

		if (rc)
			pr_warn("sev_gpu: auto-handshake client recv KMB failed (%d)\n",
				rc);
		else
			pr_info("sev_gpu: auto-handshake client installed channel %u (keyspace %u)\n",
				channel_id, keyspace);
	}
}