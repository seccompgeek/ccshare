/*
 * sev_keybroker.c -- manager/client roles for the secure key-delivery tunnel.
 *
 * Both roles operate on a VM's tunnel slot (two shared-memory rings) located in
 * a mapped view of the ivshmem BAR. They establish a mutual-TLS session over
 * those rings (via the custom sev_shm BIO) and exchange the key-delivery
 * protocol defined in sev_keybroker.h.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

#include "sev_keybroker.h"
#include "sev_shm_bio.h"

/*
 * Timeouts. The handshake/attach defaults are generous because, across two
 * VMs, the operator starts the manager and the client by hand in separate
 * shells -- the side that comes up first must wait for its peer. Override with
 * the env var KB_TIMEOUT_MS (milliseconds) if needed.
 */
#define KB_HS_TIMEOUT_DEF  120000  /* ms: wait for the peer's handshake     */
#define KB_ATTACH_DEF      120000  /* ms: client waits for manager rings    */
#define KB_IO_TIMEOUT      10000   /* ms: per message once connected        */

static int kb_timeout_ms(int def)
{
    const char *v = getenv("KB_TIMEOUT_MS");
    if (v && *v) {
        int x = atoi(v);
        if (x > 0)
            return x;
    }
    return def;
}

static void kb_ssl_err(const char *what)
{
    fprintf(stderr, "%s: ", what);
    ERR_print_errors_fp(stderr);
    fprintf(stderr, "\n");
}

static long kb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Mutual-TLS context pinned to our private CA (same policy both ends). */
static SSL_CTX *kb_make_ctx(int is_server, const struct sev_kb_certs *c)
{
    SSL_CTX *ctx = SSL_CTX_new(is_server ? TLS_server_method()
                                         : TLS_client_method());
    if (!ctx) {
        kb_ssl_err("SSL_CTX_new");
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_load_verify_locations(ctx, c->ca, NULL) != 1 ||
        SSL_CTX_use_certificate_file(ctx, c->cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, c->key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        kb_ssl_err("load credentials");
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);
    SSL_CTX_set_verify_depth(ctx, 2);
    return ctx;
}

/* Short hex fingerprint of a key, so both ends can confirm a match without
 * logging the key itself. */
static void kb_fingerprint(const uint8_t *key, size_t len, char out[17])
{
    uint8_t d[SHA256_DIGEST_LENGTH];
    int i;
    SHA256(key, len, d);
    for (i = 0; i < 8; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[16] = '\0';
}

/* ---- comm-key derivation + delivery -------------------------------------
 *
 * The shared comm key is derived from the completed mutual-TLS handshake via
 * the RFC 5705 exporter -- identical on both ends, never sent on the wire. It
 * is then handed to the driver (SEV_GPU_IOC_SET_COMM_KEY); the kernel uses it
 * to seal the in-kernel KMB exchange. No GPU key material is handled here.
 */

/* Derive the shared comm key from the handshake. Both ends use the same label
 * and empty context, so the output matches. */
static int kb_export_comm_key(SSL *ssl, uint8_t key[SEV_KB_COMM_KEY_LEN])
{
    if (SSL_export_keying_material(ssl, key, SEV_KB_COMM_KEY_LEN,
                                   SEV_KB_EXPORT_LABEL,
                                   strlen(SEV_KB_EXPORT_LABEL),
                                   NULL, 0, 0) != 1) {
        kb_ssl_err("SSL_export_keying_material");
        return -1;
    }
    return 0;
}

/* Hand the derived key to the driver. dev_fd < 0 (host self-test, file-backed
 * BAR) skips delivery -- there is no driver to receive it. */
static int kb_deliver_comm_key(int dev_fd, int vm_id, int is_manager,
                               const uint8_t key[SEV_KB_COMM_KEY_LEN])
{
    sev_gpu_ioctl_set_comm_key_t ck;
    int rc = 0;

    if (dev_fd < 0)
        return 0;

    memset(&ck, 0, sizeof(ck));
    ck.vm_id   = (uint8_t)vm_id;
    ck.key_len = SEV_KB_COMM_KEY_LEN;
    memcpy(ck.key, key, SEV_KB_COMM_KEY_LEN);
    if (ioctl(dev_fd, SEV_GPU_IOC_SET_COMM_KEY, &ck) != 0) {
        perror(is_manager ? "[mgr ] SET_COMM_KEY" : "[clnt] SET_COMM_KEY");
        rc = -1;
    }
    OPENSSL_cleanse(&ck, sizeof(ck));
    return rc;
}

/* Manager: TLS server that agrees the comm key and delivers it to the driver. */
int sev_kb_run_manager(void *bar0, int vm_id, const struct sev_kb_certs *certs,
                       int dev_fd)
{
    sev_shm_ring_t *c2m = sev_kb_c2m(bar0, vm_id); /* manager reads here  */
    sev_shm_ring_t *m2c = sev_kb_m2c(bar0, vm_id); /* manager writes here */
    SSL_CTX *ctx = kb_make_ctx(1, certs);
    SSL *ssl = NULL;
    BIO *bio;
    sev_kb_hdr_t hdr;
    uint8_t comm_key[SEV_KB_COMM_KEY_LEN];
    char fp[17];
    int hs_to = kb_timeout_ms(KB_HS_TIMEOUT_DEF);
    int rc = -1;

    if (!ctx)
        return -1;

    /* Manager owns ring init: publish empty rings for the client to attach. */
    sev_shm_ring_init(c2m, SEV_GPU_TLS_RING_CAP);
    sev_shm_ring_init(m2c, SEV_GPU_TLS_RING_CAP);
    printf("[mgr ] slot %d rings initialised, awaiting client\n", vm_id);

    ssl = SSL_new(ctx);
    bio = sev_shm_bio_new(m2c, c2m); /* tx=m2c, rx=c2m */
    if (!ssl || !bio) {
        kb_ssl_err("manager SSL/BIO");
        goto out;
    }
    SSL_set_bio(ssl, bio, bio);

    if (sev_shm_tls_handshake(ssl, 1, hs_to) != 0) {
        kb_ssl_err("manager handshake");
        goto out;
    }
    printf("[mgr ] handshake ok, peer verified, cipher=%s\n",
           SSL_get_cipher(ssl));

    /* Expect HELLO from the client (confirms it finished the handshake). */
    if (sev_shm_tls_read_full(ssl, &hdr, sizeof(hdr), KB_IO_TIMEOUT)
            != (int)sizeof(hdr) || hdr.type != SEV_KB_MSG_HELLO) {
        fprintf(stderr, "[mgr ] expected HELLO\n");
        goto out;
    }
    if (hdr.version != SEV_KB_PROTO_VERSION) {
        fprintf(stderr, "[mgr ] client proto version %u != %u\n",
                hdr.version, SEV_KB_PROTO_VERSION);
        goto out;
    }

    /* Derive the comm key from the handshake, then confirm to the client. */
    if (kb_export_comm_key(ssl, comm_key) != 0)
        goto out;
    kb_fingerprint(comm_key, sizeof(comm_key), fp);

    hdr.type    = SEV_KB_MSG_READY;
    hdr.version = SEV_KB_PROTO_VERSION;
    if (sev_shm_tls_write_all(ssl, &hdr, sizeof(hdr), KB_IO_TIMEOUT)
            != (int)sizeof(hdr)) {
        kb_ssl_err("manager send READY");
        goto out;
    }

    /*
     * Host-snoop invariant: the comm key was never written to the rings, so it
     * must not appear anywhere in the host-readable TLS region.
     */
    if (memmem(sev_kb_tls_base(bar0), SEV_GPU_TLS_REGION_SIZE,
               comm_key, sizeof(comm_key))) {
        printf("[mgr ] FAIL: comm key bytes visible in shared region!\n");
        goto out;
    }

    /* Hand the key to the driver for the in-kernel KMB exchange. */
    if (kb_deliver_comm_key(dev_fd, vm_id, 1, comm_key) != 0)
        goto out;

    printf("[mgr ] comm key agreed (fp=%s) and %s\n", fp,
           dev_fd < 0 ? "verified (host self-test, no driver)"
                      : "delivered to driver");
    rc = 0;
out:
    OPENSSL_cleanse(comm_key, sizeof(comm_key));
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    return rc;
}

/* Client: TLS client that agrees the comm key and delivers it to the driver. */
int sev_kb_run_client(void *bar0, int vm_id, const struct sev_kb_certs *certs,
                      int dev_fd)
{
    sev_shm_ring_t *c2m = sev_kb_c2m(bar0, vm_id); /* client writes here */
    sev_shm_ring_t *m2c = sev_kb_m2c(bar0, vm_id); /* client reads here  */
    SSL_CTX *ctx = kb_make_ctx(0, certs);
    SSL *ssl = NULL;
    BIO *bio;
    sev_kb_hdr_t hdr;
    uint8_t comm_key[SEV_KB_COMM_KEY_LEN];
    char fp[17];
    long deadline;
    int hs_to = kb_timeout_ms(KB_HS_TIMEOUT_DEF);
    int rc = -1;

    if (!ctx)
        return -1;

    /* Spin until the manager has published both rings (magic visible). */
    deadline = kb_now_ms() + kb_timeout_ms(KB_ATTACH_DEF);
    while (sev_shm_ring_attach(c2m) != 0 || sev_shm_ring_attach(m2c) != 0) {
        if (kb_now_ms() > deadline) {
            fprintf(stderr, "[clnt] timed out attaching to manager rings\n");
            goto out;
        }
        usleep(1000);
    }
    printf("[clnt] attached to slot %d rings\n", vm_id);

    ssl = SSL_new(ctx);
    bio = sev_shm_bio_new(c2m, m2c); /* tx=c2m, rx=m2c */
    if (!ssl || !bio) {
        kb_ssl_err("client SSL/BIO");
        goto out;
    }
    SSL_set_bio(ssl, bio, bio);

    if (sev_shm_tls_handshake(ssl, 0, hs_to) != 0) {
        kb_ssl_err("client handshake");
        goto out;
    }
    printf("[clnt] handshake ok, cipher=%s\n", SSL_get_cipher(ssl));

    /* Say HELLO (confirms we finished the handshake). */
    hdr.type    = SEV_KB_MSG_HELLO;
    hdr.version = SEV_KB_PROTO_VERSION;
    if (sev_shm_tls_write_all(ssl, &hdr, sizeof(hdr), KB_IO_TIMEOUT)
            != (int)sizeof(hdr)) {
        kb_ssl_err("client HELLO");
        goto out;
    }

    /* Derive the comm key from the handshake. */
    if (kb_export_comm_key(ssl, comm_key) != 0)
        goto out;
    kb_fingerprint(comm_key, sizeof(comm_key), fp);

    /* Wait for the manager's READY confirmation. */
    if (sev_shm_tls_read_full(ssl, &hdr, sizeof(hdr), KB_IO_TIMEOUT)
            != (int)sizeof(hdr) || hdr.type != SEV_KB_MSG_READY) {
        fprintf(stderr, "[clnt] expected READY\n");
        goto out;
    }
    if (hdr.version != SEV_KB_PROTO_VERSION) {
        fprintf(stderr, "[clnt] manager proto version %u != %u\n",
                hdr.version, SEV_KB_PROTO_VERSION);
        goto out;
    }

    /* Hand the key to the driver for the in-kernel KMB exchange. */
    if (kb_deliver_comm_key(dev_fd, vm_id, 0, comm_key) != 0)
        goto out;

    printf("[clnt] comm key agreed (fp=%s) and %s\n", fp,
           dev_fd < 0 ? "verified (host self-test, no driver)"
                      : "delivered to driver");
    rc = 0;
out:
    OPENSSL_cleanse(comm_key, sizeof(comm_key));
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    return rc;
}
