/*
 * sev_shm_ring.h -- single-producer/single-consumer byte ring in shared memory.
 *
 * One unidirectional ring lives entirely inside a shared-memory region (the
 * ivshmem control BAR in deployment, or an anonymous MAP_SHARED region in the
 * self-test). A full-duplex tunnel uses two rings (one per direction).
 *
 * Indices are free-running uint32 counters masked by (cap - 1); cap MUST be a
 * power of two. Exactly one writer touches `head`, exactly one reader touches
 * `tail`, so no locks are needed -- only acquire/release ordering on the two
 * indices, which works across processes (and across VMs over ivshmem) because
 * the backing memory is shared and coherent.
 *
 * The struct layout is fixed and self-describing (magic + cap) so two peers
 * that map the same bytes agree on it without any other coordination.
 */
#ifndef SEV_SHM_RING_H
#define SEV_SHM_RING_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define SEV_SHM_RING_MAGIC 0x53484d52u /* "SHMR" */

typedef struct {
    volatile uint32_t magic; /* SEV_SHM_RING_MAGIC once initialised      */
    volatile uint32_t cap;   /* data capacity in bytes, a power of two   */
    volatile uint32_t head;  /* producer cursor (next byte to write)     */
    volatile uint32_t tail;  /* consumer cursor (next byte to read)      */
    uint8_t  data[];         /* `cap` bytes of ring storage              */
} sev_shm_ring_t;

/* Total bytes a ring with `cap` data bytes occupies in shared memory. */
static inline size_t sev_shm_ring_bytes(uint32_t cap)
{
    return sizeof(sev_shm_ring_t) + (size_t)cap;
}

static inline int sev_shm_ring_is_pow2(uint32_t v)
{
    return v && ((v & (v - 1)) == 0);
}

/*
 * Producer-side: format an uninitialised region as an empty ring. Only one
 * peer should do this (the manager/server), before the consumer attaches.
 */
static inline int sev_shm_ring_init(sev_shm_ring_t *r, uint32_t cap)
{
    if (!r || !sev_shm_ring_is_pow2(cap))
        return -1;
    r->cap  = cap;
    r->head = 0;
    r->tail = 0;
    /* Publish magic last (release) so a peer that sees it sees a valid ring. */
    __atomic_store_n(&r->magic, SEV_SHM_RING_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

/* Consumer-side: returns 0 once the producer has published a valid ring. */
static inline int sev_shm_ring_attach(sev_shm_ring_t *r)
{
    if (!r)
        return -1;
    if (__atomic_load_n(&r->magic, __ATOMIC_ACQUIRE) != SEV_SHM_RING_MAGIC)
        return -1;
    if (!sev_shm_ring_is_pow2(r->cap))
        return -1;
    return 0;
}

/* Bytes currently available to read. */
static inline uint32_t sev_shm_ring_used(const sev_shm_ring_t *r)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

/*
 * Producer: write up to `len` bytes, returning the number actually written
 * (0 if the ring is full). Never blocks.
 */
static inline uint32_t sev_shm_ring_write(sev_shm_ring_t *r,
                                          const void *buf, uint32_t len)
{
    uint32_t cap   = r->cap;
    uint32_t head  = __atomic_load_n(&r->head, __ATOMIC_RELAXED); /* we own it */
    uint32_t tail  = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t space = cap - (head - tail);
    uint32_t n     = (len < space) ? len : space;
    uint32_t i;

    for (i = 0; i < n; i++)
        r->data[(head + i) & (cap - 1)] = ((const uint8_t *)buf)[i];

    /* Publish the new head only after the bytes are in place. */
    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return n;
}

/*
 * Consumer: read up to `len` bytes, returning the number actually read
 * (0 if the ring is empty). Never blocks.
 */
static inline uint32_t sev_shm_ring_read(sev_shm_ring_t *r,
                                         void *buf, uint32_t len)
{
    uint32_t cap  = r->cap;
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED); /* we own it */
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t avail = head - tail;
    uint32_t n    = (len < avail) ? len : avail;
    uint32_t i;

    for (i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = r->data[(tail + i) & (cap - 1)];

    /* Release the consumed space only after the bytes are copied out. */
    __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}

/*
 * Consumer: copy up to `len` available bytes WITHOUT consuming them (tail is
 * left untouched). Used to inspect raw on-the-wire bytes -- e.g. to prove that
 * what transits the shared ring is TLS ciphertext, not plaintext.
 */
static inline uint32_t sev_shm_ring_peek(const sev_shm_ring_t *r,
                                         void *buf, uint32_t len)
{
    uint32_t cap  = r->cap;
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t avail = head - tail;
    uint32_t n    = (len < avail) ? len : avail;
    uint32_t i;

    for (i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = r->data[(tail + i) & (cap - 1)];
    return n;
}

#endif /* SEV_SHM_RING_H */
