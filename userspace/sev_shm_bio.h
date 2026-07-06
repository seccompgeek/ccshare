/*
 * sev_shm_bio.h -- a custom OpenSSL BIO that carries TLS records over a pair of
 * shared-memory rings (one TX, one RX) instead of a socket.
 *
 * This is what makes a mutual-TLS session run across the ivshmem channel: the
 * SSL engine calls BIO_write/BIO_read, which push/pop ciphertext bytes on the
 * shared rings. The untrusted host can observe the rings but only sees TLS
 * records (ciphertext), never the plaintext key material exchanged inside.
 *
 * The transport is non-blocking: when a ring is empty/full the BIO reports a
 * retry condition, so callers must drive the handshake/IO with the helper
 * loops below (which poll, since shared memory has no pollable fd).
 */
#ifndef SEV_SHM_BIO_H
#define SEV_SHM_BIO_H

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include "sev_shm_ring.h"

/* Create a BIO whose writes go to `tx` and whose reads come from `rx`.
 * Returns NULL on failure. Ownership of the rings stays with the caller. */
BIO *sev_shm_bio_new(sev_shm_ring_t *tx, sev_shm_ring_t *rx);

/* Drive a TLS handshake to completion over a polled shm BIO.
 * is_server selects SSL_accept vs SSL_connect. timeout_ms bounds the wait.
 * Returns 0 on success, -1 on error/timeout. */
int sev_shm_tls_handshake(SSL *ssl, int is_server, int timeout_ms);

/* Blocking-style helpers that poll the non-blocking BIO until done.
 * Return >0 bytes transferred, 0 on clean close, -1 on error/timeout. */
int sev_shm_tls_write_all(SSL *ssl, const void *buf, int len, int timeout_ms);
int sev_shm_tls_read_full(SSL *ssl, void *buf, int len, int timeout_ms);

#endif /* SEV_SHM_BIO_H */
