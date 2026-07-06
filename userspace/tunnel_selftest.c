/*
 * tunnel_selftest.c -- host-runnable proof of the SEV-GPU secure tunnel core.
 *
 * Two processes (forked) share an anonymous MAP_SHARED region that holds two
 * SPSC rings -- exactly the shape of the per-VM tunnel slice that will live in
 * the ivshmem control BAR. The parent plays the manager (TLS server), the child
 * plays the guest VM (TLS client). They run a *mutual*-TLS handshake (each side
 * presents a CA-signed cert and verifies the peer) entirely over the shared
 * rings, then the manager delivers a placeholder GPU key blob ("KMB") which the
 * client decrypts and acknowledges.
 *
 * To make the security property concrete, the client peeks the raw ring bytes
 * before decrypting and asserts the plaintext key marker is absent -- i.e. an
 * untrusted host snooping the ivshmem region would see only TLS ciphertext.
 *
 * No VM, kernel module, or GPU is required: this validates the novel mechanism
 * on the host before it is wired to real ivshmem memory.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "sev_shm_ring.h"
#include "sev_shm_bio.h"

#define RING_CAP    (1u << 16)          /* 64 KiB data per ring          */
#define HS_TIMEOUT  5000                /* ms                            */
#define IO_TIMEOUT  5000                /* ms                            */

/* The "GPU key material blob" the manager hands the guest. The marker prefix
 * lets the client prove the plaintext never appears on the raw ring. */
#define KMB_MARKER  "SEV-KMB:"
static const char KMB_BLOB[] =
    KMB_MARKER "0123456789ABCDEF0123456789ABCDEF-aes256-channel-key";
#define KMB_ACK     "KMB-ACK"

struct certs {
    const char *ca;
    const char *cert;
    const char *key;
};

static void ssl_die(const char *what)
{
    fprintf(stderr, "%s: ", what);
    ERR_print_errors_fp(stderr);
    fprintf(stderr, "\n");
}

/* Build a context that requires and verifies a peer certificate (mutual TLS),
 * pinning trust to our private CA. */
static SSL_CTX *make_ctx(int is_server, const struct certs *c)
{
    SSL_CTX *ctx = SSL_CTX_new(is_server ? TLS_server_method()
                                         : TLS_client_method());
    if (!ctx) {
        ssl_die("SSL_CTX_new");
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_load_verify_locations(ctx, c->ca, NULL) != 1) {
        ssl_die("load CA");
        goto fail;
    }
    if (SSL_CTX_use_certificate_file(ctx, c->cert, SSL_FILETYPE_PEM) != 1) {
        ssl_die("use cert");
        goto fail;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, c->key, SSL_FILETYPE_PEM) != 1) {
        ssl_die("use key");
        goto fail;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        ssl_die("check key");
        goto fail;
    }

    /* Both ends verify the peer and fail the handshake if it is missing. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);
    SSL_CTX_set_verify_depth(ctx, 2);
    return ctx;

fail:
    SSL_CTX_free(ctx);
    return NULL;
}

/* Child: the guest VM. tx = client->manager ring, rx = manager->client ring. */
static int run_client(sev_shm_ring_t *tx, sev_shm_ring_t *rx,
                      const struct certs *c)
{
    SSL_CTX *ctx = make_ctx(0, c);
    SSL *ssl;
    BIO *bio;
    char raw[64], buf[256];
    uint32_t got;
    int n, rc = 1;

    if (!ctx)
        return 1;

    ssl = SSL_new(ctx);
    bio = sev_shm_bio_new(tx, rx);
    if (!ssl || !bio) {
        ssl_die("client SSL/BIO");
        goto out;
    }
    SSL_set_bio(ssl, bio, bio);

    if (sev_shm_tls_handshake(ssl, 0, HS_TIMEOUT) != 0) {
        ssl_die("client handshake");
        goto out;
    }
    printf("[client] handshake ok, cipher=%s\n", SSL_get_cipher(ssl));

    /* Wiretap: look at the raw ring bytes the (untrusted) host would see. */
    while (sev_shm_ring_used(rx) == 0)
        usleep(100);
    got = sev_shm_ring_peek(rx, raw, sizeof(raw));
    if (memmem(raw, got, KMB_MARKER, strlen(KMB_MARKER)) != NULL) {
        printf("[client] FAIL: plaintext key marker visible on raw ring!\n");
        goto out;
    }
    printf("[client] raw ring is ciphertext (first 16B):");
    for (uint32_t i = 0; i < got && i < 16; i++)
        printf(" %02x", (unsigned char)raw[i]);
    printf("\n");

    /* Decrypt the delivered key blob. */
    n = sev_shm_tls_read_full(ssl, buf, (int)sizeof(KMB_BLOB), IO_TIMEOUT);
    if (n != (int)sizeof(KMB_BLOB) || memcmp(buf, KMB_BLOB, n) != 0) {
        printf("[client] FAIL: key blob mismatch (n=%d)\n", n);
        goto out;
    }
    printf("[client] decrypted KMB blob (%d bytes), delivering ACK\n", n);

    if (sev_shm_tls_write_all(ssl, KMB_ACK, (int)sizeof(KMB_ACK),
                              IO_TIMEOUT) != (int)sizeof(KMB_ACK)) {
        ssl_die("client ack");
        goto out;
    }
    rc = 0;

out:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    return rc;
}

/* Parent: the manager. tx = manager->client ring, rx = client->manager ring. */
static int run_server(sev_shm_ring_t *tx, sev_shm_ring_t *rx,
                      const struct certs *c)
{
    SSL_CTX *ctx = make_ctx(1, c);
    SSL *ssl;
    BIO *bio;
    char buf[64];
    int n, rc = 1;

    if (!ctx)
        return 1;

    ssl = SSL_new(ctx);
    bio = sev_shm_bio_new(tx, rx);
    if (!ssl || !bio) {
        ssl_die("server SSL/BIO");
        goto out;
    }
    SSL_set_bio(ssl, bio, bio);

    if (sev_shm_tls_handshake(ssl, 1, HS_TIMEOUT) != 0) {
        ssl_die("server handshake");
        goto out;
    }
    printf("[server] handshake ok, peer verified, cipher=%s\n",
           SSL_get_cipher(ssl));

    /* Deliver the GPU key blob over the encrypted channel. */
    if (sev_shm_tls_write_all(ssl, KMB_BLOB, (int)sizeof(KMB_BLOB),
                              IO_TIMEOUT) != (int)sizeof(KMB_BLOB)) {
        ssl_die("server send KMB");
        goto out;
    }
    printf("[server] delivered KMB blob (%zu bytes)\n", sizeof(KMB_BLOB));

    n = sev_shm_tls_read_full(ssl, buf, (int)sizeof(KMB_ACK), IO_TIMEOUT);
    if (n != (int)sizeof(KMB_ACK) || memcmp(buf, KMB_ACK, n) != 0) {
        printf("[server] FAIL: bad ack (n=%d)\n", n);
        goto out;
    }
    printf("[server] received ACK, key delivery confirmed\n");
    rc = 0;

out:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "certs";
    char ca[512], scert[512], skey[512], ccert[512], ckey[512];
    struct certs server_c, client_c;
    size_t ring_bytes = sev_shm_ring_bytes(RING_CAP);
    size_t region = 2 * ring_bytes;
    void *base;
    sev_shm_ring_t *c2m, *m2c;
    pid_t pid;
    int crc = 1, srv;

    snprintf(ca,    sizeof(ca),    "%s/ca_cert.pem",     dir);
    snprintf(scert, sizeof(scert), "%s/server_cert.pem", dir);
    snprintf(skey,  sizeof(skey),  "%s/server_key.pem",  dir);
    snprintf(ccert, sizeof(ccert), "%s/client_cert.pem", dir);
    snprintf(ckey,  sizeof(ckey),  "%s/client_key.pem",  dir);
    server_c = (struct certs){ ca, scert, skey };
    client_c = (struct certs){ ca, ccert, ckey };

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    /* Shared region modelling the ivshmem per-VM tunnel slice. */
    base = mmap(NULL, region, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    c2m = (sev_shm_ring_t *)base;                       /* guest -> manager */
    m2c = (sev_shm_ring_t *)((char *)base + ring_bytes); /* manager -> guest */
    sev_shm_ring_init(c2m, RING_CAP);
    sev_shm_ring_init(m2c, RING_CAP);

    printf("tunnel_selftest: region=%zu bytes, ring_cap=%u, certs=%s\n",
           region, RING_CAP, dir);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child = guest VM / TLS client. */
        int rc = run_client(c2m, m2c, &client_c);
        _exit(rc);
    }

    /* Parent = manager / TLS server. */
    srv = run_server(m2c, c2m, &server_c);

    if (waitpid(pid, &crc, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    crc = WIFEXITED(crc) ? WEXITSTATUS(crc) : 1;

    if (srv == 0 && crc == 0) {
        printf("\nRESULT: PASS -- mutual TLS + key delivery over shared-memory rings\n");
        return 0;
    }
    printf("\nRESULT: FAIL (server=%d client=%d)\n", srv, crc);
    return 1;
}
