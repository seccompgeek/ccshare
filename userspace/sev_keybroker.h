/*
 * sev_keybroker.h -- userspace half of the SEV-GPU secure key bootstrap.
 *
 * The keybroker's ONLY job is to establish trust between the manager and a
 * client VM and agree a shared communication key, then hand that key down to
 * the kernel driver. It does this by running a mutual-TLS handshake over a pair
 * of shared-memory rings inside the ivshmem control BAR (the untrusted host can
 * read the BAR but sees only TLS ciphertext) and deriving a symmetric key from
 * the completed handshake via the TLS exporter (RFC 5705) -- identical on both
 * ends, never transmitted.
 *
 * The key is delivered into the driver with SEV_GPU_IOC_SET_COMM_KEY. From
 * there the kernel uses it to seal the in-kernel exchange of the GPU channel
 * key material (CC_KMB). The KMB is never handled in userspace.
 *
 * This header defines the small handshake-confirmation frames and helpers that
 * locate a VM's tunnel rings within a mapped view of the BAR.
 */
#ifndef SEV_KEYBROKER_H
#define SEV_KEYBROKER_H

#include <stdint.h>
#include <stddef.h>

#include "../kernel/sev_gpu_manager.h"
#include "sev_shm_ring.h"

/* ---- comm-key derivation parameters ---- */

/* Exporter label: both ends must use the same label (and no context) so the
 * derived key matches. Bump the suffix on any protocol change. */
#define SEV_KB_EXPORT_LABEL "sev-gpu manager-client comm key v1"
#define SEV_KB_COMM_KEY_LEN  SEV_GPU_COMM_KEY_LEN  /* 32 (AES-256) */

/* ---- on-wire confirmation frames (sent inside the encrypted TLS stream) ----
 *
 * No key material crosses the wire: the comm key is derived from the handshake.
 * These tiny frames just confirm both ends reached the post-handshake state
 * (and agree on the protocol version) before each installs the derived key. */

#define SEV_KB_MSG_HELLO 1u  /* client -> manager: handshake complete       */
#define SEV_KB_MSG_READY 2u  /* manager -> client: confirmed, install key   */

typedef struct {
    uint32_t type;
    uint32_t version;        /* protocol/exporter version guard */
} __attribute__((packed)) sev_kb_hdr_t;

#define SEV_KB_PROTO_VERSION 1u

/* ---- tunnel-slot geometry within a mapped BAR view ---- */

/* Base of the whole TLS region, given a pointer to BAR offset 0. */
static inline void *sev_kb_tls_base(void *bar0)
{
    return (char *)bar0 + SEV_GPU_TLS_REGION_OFF;
}

/* This VM's slot within the TLS region. */
static inline void *sev_kb_slot(void *bar0, int vm_id)
{
    return (char *)sev_kb_tls_base(bar0) + (size_t)vm_id * SEV_GPU_TLS_SLOT_STRIDE;
}

/* Client->manager ring for a VM slot. */
static inline sev_shm_ring_t *sev_kb_c2m(void *bar0, int vm_id)
{
    return (sev_shm_ring_t *)((char *)sev_kb_slot(bar0, vm_id) + SEV_GPU_TLS_C2M_OFF);
}

/* Manager->client ring for a VM slot. */
static inline sev_shm_ring_t *sev_kb_m2c(void *bar0, int vm_id)
{
    return (sev_shm_ring_t *)((char *)sev_kb_slot(bar0, vm_id) + SEV_GPU_TLS_M2C_OFF);
}

/* ---- roles (implemented in sev_keybroker.c) ---- */

struct sev_kb_certs {
    const char *ca;
    const char *cert;
    const char *key;
};

/*
 * Run one key bootstrap over the VM's tunnel slot. `bar0` points at the mapped
 * BAR offset 0 (so the TLS region is at bar0 + SEV_GPU_TLS_REGION_OFF).
 * `dev_fd` is the open /dev/sev_gpu_manager descriptor used to deliver the
 * derived comm key via SEV_GPU_IOC_SET_COMM_KEY, or -1 for the host self-test
 * (file-backed BAR, no driver) in which case the key is only fingerprinted.
 * Returns 0 on success, -1 on failure.
 *
 * The manager initialises the slot's rings and runs the TLS server; the client
 * attaches and runs the TLS client. Both authenticate with the pinned CA,
 * derive the comm key from the handshake (TLS exporter), confirm liveness with
 * a HELLO/READY exchange, and deliver the key to the driver. No key material is
 * ever placed on the rings.
 */
int sev_kb_run_manager(void *bar0, int vm_id, const struct sev_kb_certs *certs,
                       int dev_fd);
int sev_kb_run_client(void *bar0, int vm_id, const struct sev_kb_certs *certs,
                      int dev_fd);

#endif /* SEV_KEYBROKER_H */
