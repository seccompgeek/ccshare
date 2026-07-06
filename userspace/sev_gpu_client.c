/*
 * sev_gpu_client.c
 * 
 * User-space library implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include "sev_gpu_client.h"

/* Open connection to manager */
int sev_gpu_client_open(sev_gpu_client_t *client, uint8_t vm_id, const char *vm_name) {
    sev_gpu_ioctl_register_vm_t reg;
    sev_gpu_ioctl_get_shmem_t shmem_info;
    int ret;
    
    if (!client || !vm_name) {
        fprintf(stderr, "[-] Invalid arguments\n");
        return -1;
    }
    
    /* Open device */
    client->dev_fd = open("/dev/sev_gpu_manager", O_RDWR);
    if (client->dev_fd < 0) {
        perror("[-] Failed to open /dev/sev_gpu_manager");
        return -1;
    }
    
    client->vm_id = vm_id;
    client->data_fd = -1;
    strncpy(client->vm_name, vm_name, sizeof(client->vm_name) - 1);
    
    /* Register with manager */
    reg.vm_id = vm_id;
    strncpy(reg.vm_name, vm_name, sizeof(reg.vm_name) - 1);
    reg.vm_pid = getpid();
    
    ret = ioctl(client->dev_fd, SEV_GPU_IOC_REGISTER_VM, &reg);
    if (ret < 0) {
        perror("[-] Failed to register VM");
        close(client->dev_fd);
        return -1;
    }
    
    printf("[+] Registered as VM %d (%s)\n", vm_id, vm_name);
    
    /* Get shared memory info */
    ret = ioctl(client->dev_fd, SEV_GPU_IOC_GET_SHMEM, &shmem_info);
    if (ret < 0) {
        perror("[-] Failed to get shared memory info");
        close(client->dev_fd);
        return -1;
    }
    
    printf("[+] Shared memory: phys=0x%llx size=%llu\n",
           shmem_info.phys_addr, shmem_info.size);
    
    /*
     * Map the control plane (header + request/grant rings) -- the whole
     * control BAR2 at offset 0. The private data path is a separate device
     * (mapped below), so the control device carries only metadata + rings.
     */
    client->shmem_base = mmap(NULL, SEV_GPU_CONTROL_SIZE, PROT_READ | PROT_WRITE,
                               MAP_SHARED, client->dev_fd, 0);
    if (client->shmem_base == MAP_FAILED) {
        perror("[-] Failed to mmap control plane");
        close(client->dev_fd);
        return -1;
    }
    
    client->shmem_size = SEV_GPU_CONTROL_SIZE;
    client->header = (sev_gpu_shmem_header_t *)client->shmem_base;

    /* Verify header */
    if (client->header->magic != 0xDEADBEEFCAFEBABE) {
        fprintf(stderr, "[-] Invalid shared memory header (manager not ready?)\n");
        munmap(client->shmem_base, client->shmem_size);
        close(client->dev_fd);
        return -1;
    }

    /* Learn our role and effective slot index from the kernel. */
    {
        sev_gpu_ioctl_get_role_t role;

        client->is_manager = 0;
        client->ivposition = -1;
        if (ioctl(client->dev_fd, SEV_GPU_IOC_GET_ROLE, &role) == 0) {
            client->is_manager = role.is_manager;
            client->ivposition = role.ivposition;
            /* Kernel adopts ivposition as the client slot when valid. */
            if (!role.is_manager &&
                role.ivposition > 0 && role.ivposition < SEV_GPU_MAX_VMS)
                client->vm_id = (uint8_t)role.ivposition;
        }
    }

    /*
     * Open and map our PRIVATE per-VM data device. This is a separate
     * ivshmem-plain device that QEMU exposes only to this VM (+ the manager),
     * so the isolation boundary is the hypervisor, not cooperative bounds.
     * A client VM sees exactly one such device, enumerated as data0.
     */
    {
        const char *path = "/dev/sev_gpu_data0";
        sev_gpu_ioctl_data_info_t info;

        client->data_fd           = -1;
        client->data_base         = NULL;
        client->data_size         = 0;
        client->data_payload_off  = 0;
        client->data_payload_size = 0;
        client->data_pool_index   = 0;

        client->data_fd = open(path, O_RDWR);
        if (client->data_fd < 0) {
            /* Not fatal: control plane still works; data path unavailable. */
            fprintf(stderr, "[!] No private data device %s (%s)\n",
                    path, strerror(errno));
        } else if (ioctl(client->data_fd, SEV_GPU_IOC_DATA_INFO, &info) == 0 &&
                   info.region_size > 0) {
            void *p = mmap(NULL, info.region_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, client->data_fd, 0);
            if (p == MAP_FAILED) {
                perror("[-] Failed to mmap private data device");
                close(client->data_fd);
                client->data_fd = -1;
            } else {
                client->data_base         = p;
                client->data_size         = info.region_size;
                client->data_payload_off  = info.payload_off;
                client->data_payload_size = info.payload_size;
                client->data_pool_index   = info.pool_index;
                printf("[+] Mapped private data dev%u size=%llu payload=%llu@0x%llx owner=%u state=%u\n",
                       info.pool_index,
                       (unsigned long long)info.region_size,
                       (unsigned long long)info.payload_size,
                       (unsigned long long)info.payload_off,
                       info.owner_vm_id, info.state);
            }
        } else {
            fprintf(stderr, "[!] DATA_INFO failed on %s (%s)\n",
                    path, strerror(errno));
            close(client->data_fd);
            client->data_fd = -1;
        }
    }

    printf("[+] Successfully mapped shared memory (role=%s, ivpos=%d, slot=%u)\n",
           client->is_manager ? "manager" : "client",
           client->ivposition, client->vm_id);
    return 0;
}

/* Close connection */
void sev_gpu_client_close(sev_gpu_client_t *client) {
    if (!client)
        return;
    
    if (client->data_base) {
        munmap(client->data_base, client->data_size);
        client->data_base = NULL;
    }

    if (client->data_fd >= 0) {
        close(client->data_fd);
        client->data_fd = -1;
    }

    if (client->shmem_base) {
        munmap(client->shmem_base, client->shmem_size);
        client->shmem_base = NULL;
    }
    
    if (client->dev_fd >= 0) {
        close(client->dev_fd);
        client->dev_fd = -1;
    }
}

/* Request GPU access */
int sev_gpu_client_request_gpu(sev_gpu_client_t *client, uint32_t duration_us, uint8_t priority) {
    sev_gpu_ioctl_request_gpu_t req;
    int ret;
    
    if (!client || client->dev_fd < 0) {
        fprintf(stderr, "[-] Client not connected\n");
        return -1;
    }
    
    req.vm_id = client->vm_id;
    req.duration_us = duration_us;
    req.priority = priority;
    
    ret = ioctl(client->dev_fd, SEV_GPU_IOC_REQUEST_GPU, &req);
    if (ret < 0) {
        perror("[-] Failed to request GPU");
        return -1;
    }
    
    printf("[+] GPU requested for %u us (priority %d)\n", duration_us, priority);
    return 0;
}

/* Check if GPU has been granted (non-blocking) */
int sev_gpu_client_check_grant(sev_gpu_client_t *client, gpu_grant_t *grant) {
    gpu_grant_t *grant_slot;

    if (!client || !grant || client->dev_fd < 0) {
        fprintf(stderr, "[-] Invalid arguments\n");
        return -1;
    }

    /* Read this VM's grant slot (per-VM, indexed by vm_id). */
    grant_slot = (gpu_grant_t *)((char *)client->shmem_base +
                                 client->header->grant_region_off +
                                 (size_t)client->vm_id * sizeof(gpu_grant_t));

    if (grant_slot->status == GPU_STATUS_GRANTED &&
        grant_slot->vm_id == client->vm_id) {
        *grant = *grant_slot;
        return 1;
    }

    return 0;  /* Still waiting */
}

/* Wait for GPU grant (blocking, interrupt-driven via the kernel module) */
int sev_gpu_client_wait_grant(sev_gpu_client_t *client, gpu_grant_t *grant, int timeout_ms) {
    sev_gpu_ioctl_wait_grant_t wg;
    int ret;

    if (!client || !grant || client->dev_fd < 0) {
        fprintf(stderr, "[-] Invalid arguments\n");
        return -1;
    }

    memset(&wg, 0, sizeof(wg));
    wg.vm_id = client->vm_id;
    wg.timeout_ms = timeout_ms;

    ret = ioctl(client->dev_fd, SEV_GPU_IOC_WAIT_GRANT, &wg);
    if (ret < 0) {
        if (errno == ETIMEDOUT)
            fprintf(stderr, "[-] GPU grant timeout\n");
        else
            perror("[-] Failed to wait for grant");
        return -1;
    }

    *grant = wg.grant;
    printf("[+] GPU granted!\n");
    return 0;
}

/* Release GPU */
int sev_gpu_client_release_gpu(sev_gpu_client_t *client) {
    sev_gpu_ioctl_release_gpu_t rel;
    int ret;
    
    if (!client || client->dev_fd < 0) {
        fprintf(stderr, "[-] Client not connected\n");
        return -1;
    }
    
    rel.vm_id = client->vm_id;
    
    ret = ioctl(client->dev_fd, SEV_GPU_IOC_RELEASE_GPU, &rel);
    if (ret < 0) {
        perror("[-] Failed to release GPU");
        return -1;
    }
    
    printf("[+] GPU released\n");
    return 0;
}

/* Get shared memory info */
int sev_gpu_client_get_shmem(sev_gpu_client_t *client, uint64_t *phys_addr, size_t *size) {
    if (!client || !phys_addr || !size) {
        return -1;
    }
    
    *phys_addr = 0;  /* Would need ioctl to get actual physical address */
    *size = client->shmem_size;
    
    return 0;
}

/* Get this client's private data region payload (mapped at open time). */
int sev_gpu_client_get_data_region(sev_gpu_client_t *client, void **base, size_t *size) {
    if (!client || !base || !size)
        return -1;

    if (!client->data_base || client->data_payload_size == 0)
        return -1;

    *base = (char *)client->data_base + client->data_payload_off;
    *size = client->data_payload_size;
    return 0;
}

/* Write request to shared memory (this VM's per-VM slot) */
int sev_gpu_client_write_request(sev_gpu_client_t *client, const gpu_request_t *req) {
    gpu_request_t *req_slot;

    if (!client || !req || client->dev_fd < 0) {
        fprintf(stderr, "[-] Invalid arguments\n");
        return -1;
    }

    req_slot = (gpu_request_t *)((char *)client->shmem_base +
                                client->header->request_region_off +
                                (size_t)client->vm_id * sizeof(gpu_request_t));
    *req_slot = *req;
    return 0;
}

/* Read grant from shared memory (this VM's per-VM slot) */
int sev_gpu_client_read_grant(sev_gpu_client_t *client, gpu_grant_t *grant) {
    gpu_grant_t *grant_slot;

    if (!client || !grant || client->dev_fd < 0) {
        fprintf(stderr, "[-] Invalid arguments\n");
        return -1;
    }

    grant_slot = (gpu_grant_t *)((char *)client->shmem_base +
                                 client->header->grant_region_off +
                                 (size_t)client->vm_id * sizeof(gpu_grant_t));
    *grant = *grant_slot;
    return 0;
}
