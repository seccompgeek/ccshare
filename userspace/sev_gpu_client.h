/*
 * sev_gpu_client.h
 * 
 * User-space library for communicating with sev_gpu_manager kernel module
 * Used by VMs to request GPU access
 */

#ifndef SEV_GPU_CLIENT_H
#define SEV_GPU_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include "sev_gpu_manager.h"

typedef struct {
    int dev_fd;                     /* File descriptor to /dev/sev_gpu_manager */
    void *shmem_base;               /* Mapped control plane (header + rings)   */
    size_t shmem_size;              /* Control-plane mapping length            */
    sev_gpu_shmem_header_t *header; /* Header pointer (== shmem_base)          */
    int data_fd;                    /* fd to /dev/sev_gpu_dataN (-1 if none)   */
    void *data_base;                /* Mapped whole private region (hdr+payload)*/
    size_t data_size;               /* Private region length (region_size)     */
    uint64_t data_payload_off;      /* Payload offset within the region        */
    uint64_t data_payload_size;     /* Payload bytes                           */
    uint32_t data_pool_index;       /* Data device/pool index                  */
    uint8_t vm_id;                  /* Effective slot index (ivposition or requested) */
    char vm_name[32];
    uint8_t is_manager;             /* 1 if this node is the GPU manager */
    int32_t ivposition;             /* ivshmem peer id (-1 if unavailable) */
} sev_gpu_client_t;

/**
 * Open connection to sev_gpu_manager
 * 
 * Returns 0 on success, -1 on error
 */
int sev_gpu_client_open(sev_gpu_client_t *client, uint8_t vm_id, const char *vm_name);

/**
 * Close connection
 */
void sev_gpu_client_close(sev_gpu_client_t *client);

/**
 * Request GPU access for specified duration
 * 
 * duration_us: Requested GPU time in microseconds
 * priority: Priority level (0-255, higher = more urgent)
 * 
 * Returns 0 on success, -1 on error
 */
int sev_gpu_client_request_gpu(sev_gpu_client_t *client, uint32_t duration_us, uint8_t priority);

/**
 * Check if GPU has been granted
 * 
 * grant: Pointer to gpu_grant_t structure to fill
 * 
 * Returns 1 if granted, 0 if still waiting, -1 on error
 */
int sev_gpu_client_check_grant(sev_gpu_client_t *client, gpu_grant_t *grant);

/**
 * Wait for GPU grant (blocking)
 * 
 * grant: Pointer to gpu_grant_t structure to fill
 * timeout_ms: Timeout in milliseconds (-1 = infinite)
 * 
 * Returns 0 on success, -1 on error
 */
int sev_gpu_client_wait_grant(sev_gpu_client_t *client, gpu_grant_t *grant, int timeout_ms);

/**
 * Release GPU
 */
int sev_gpu_client_release_gpu(sev_gpu_client_t *client);

/**
 * Get shared memory info
 */
int sev_gpu_client_get_shmem(sev_gpu_client_t *client, uint64_t *phys_addr, size_t *size);

/**
 * Get this client's private data region payload (mapped at open time).
 *
 * base/size point at the per-VM PRIVATE region's payload. The region is a
 * separate ivshmem-plain device (/dev/sev_gpu_dataN) that QEMU exposes only to
 * this VM and the manager -- spatial isolation is enforced by the hypervisor,
 * not by cooperative mmap bounds. Returns 0 on success, -1 if no region.
 */
int sev_gpu_client_get_data_region(sev_gpu_client_t *client, void **base, size_t *size);

/**
 * Write request to shared memory request channel
 */
int sev_gpu_client_write_request(sev_gpu_client_t *client, const gpu_request_t *req);

/**
 * Read grant from shared memory grant channel
 */
int sev_gpu_client_read_grant(sev_gpu_client_t *client, gpu_grant_t *grant);

#endif /* SEV_GPU_CLIENT_H */
