/*
 * sev_shm_bio.c -- implementation of the shared-memory OpenSSL BIO and the
 * polled handshake/IO helper loops.
 */

#include <openssl/err.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "sev_shm_bio.h"

/* Per-BIO state: the two rings this endpoint pushes to / pulls from. */
typedef struct {
    sev_shm_ring_t *tx;
    sev_shm_ring_t *rx;
} sev_shm_bio_ctx_t;

/* Poll cadence while waiting for the peer to produce/consume ring bytes. */
#define SEV_SHM_BIO_POLL_US 50

static int shm_bio_write(BIO *b, const char *buf, int len)
{
    sev_shm_bio_ctx_t *c = BIO_get_data(b);
    uint32_t n;

    BIO_clear_retry_flags(b);
    if (len <= 0)
        return 0;

    n = sev_shm_ring_write(c->tx, buf, (uint32_t)len);
    if (n == 0) {
        /* Ring full -- ask the SSL engine to retry this write later. */
        BIO_set_retry_write(b);
        return -1;
    }
    return (int)n;
}

static int shm_bio_read(BIO *b, char *buf, int len)
{
    sev_shm_bio_ctx_t *c = BIO_get_data(b);
    uint32_t n;

    BIO_clear_retry_flags(b);
    if (len <= 0)
        return 0;

    n = sev_shm_ring_read(c->rx, buf, (uint32_t)len);
    if (n == 0) {
        /* Ring empty -- ask the SSL engine to retry this read later. */
        BIO_set_retry_read(b);
        return -1;
    }
    return (int)n;
}

static long shm_bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    (void)b; (void)num; (void)ptr;
    switch (cmd) {
    case BIO_CTRL_FLUSH:      /* nothing buffered beyond the ring */
        return 1;
    case BIO_CTRL_PUSH:
    case BIO_CTRL_POP:
        return 0;
    default:
        return 0;
    }
}

static int shm_bio_create(BIO *b)
{
    BIO_set_init(b, 1);
    BIO_set_data(b, NULL);
    return 1;
}

static int shm_bio_destroy(BIO *b)
{
    sev_shm_bio_ctx_t *c;

    if (b == NULL)
        return 0;
    c = BIO_get_data(b);
    if (c != NULL) {
        OPENSSL_free(c);
        BIO_set_data(b, NULL);
    }
    BIO_set_init(b, 0);
    return 1;
}

static BIO_METHOD *sev_shm_bio_method(void)
{
    static BIO_METHOD *meth = NULL;

    if (meth == NULL) {
        int type = BIO_get_new_index() | BIO_TYPE_SOURCE_SINK;
        meth = BIO_meth_new(type, "sev_shm_bio");
        if (meth == NULL)
            return NULL;
        BIO_meth_set_write(meth, shm_bio_write);
        BIO_meth_set_read(meth, shm_bio_read);
        BIO_meth_set_ctrl(meth, shm_bio_ctrl);
        BIO_meth_set_create(meth, shm_bio_create);
        BIO_meth_set_destroy(meth, shm_bio_destroy);
    }
    return meth;
}

BIO *sev_shm_bio_new(sev_shm_ring_t *tx, sev_shm_ring_t *rx)
{
    BIO_METHOD *meth = sev_shm_bio_method();
    sev_shm_bio_ctx_t *c;
    BIO *b;

    if (meth == NULL || tx == NULL || rx == NULL)
        return NULL;

    b = BIO_new(meth);
    if (b == NULL)
        return NULL;

    c = OPENSSL_malloc(sizeof(*c));
    if (c == NULL) {
        BIO_free(b);
        return NULL;
    }
    c->tx = tx;
    c->rx = rx;
    BIO_set_data(b, c);
    return b;
}

/* Monotonic milliseconds for timeout bookkeeping. */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void poll_pause(void)
{
    struct timespec ts = { 0, SEV_SHM_BIO_POLL_US * 1000 };
    nanosleep(&ts, NULL);
}

int sev_shm_tls_handshake(SSL *ssl, int is_server, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        int rc = is_server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (rc == 1)
            return 0;

        switch (SSL_get_error(ssl, rc)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            if (now_ms() > deadline)
                return -1;
            poll_pause();
            continue;
        default:
            return -1;
        }
    }
}

int sev_shm_tls_write_all(SSL *ssl, const void *buf, int len, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    int off = 0;

    while (off < len) {
        int rc = SSL_write(ssl, (const char *)buf + off, len - off);
        if (rc > 0) {
            off += rc;
            continue;
        }
        switch (SSL_get_error(ssl, rc)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            if (now_ms() > deadline)
                return -1;
            poll_pause();
            continue;
        default:
            return -1;
        }
    }
    return off;
}

int sev_shm_tls_read_full(SSL *ssl, void *buf, int len, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    int off = 0;

    while (off < len) {
        int rc = SSL_read(ssl, (char *)buf + off, len - off);
        if (rc > 0) {
            off += rc;
            continue;
        }
        switch (SSL_get_error(ssl, rc)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            if (now_ms() > deadline)
                return -1;
            poll_pause();
            continue;
        case SSL_ERROR_ZERO_RETURN:
            return off; /* peer closed cleanly */
        default:
            return -1;
        }
    }
    return off;
}
