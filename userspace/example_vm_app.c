/*
 * example_vm_app.c
 * 
 * Example: How a VM application requests GPU and runs workload
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sev_gpu_client.h"

int main(int argc, char **argv) {
    sev_gpu_client_t client;
    gpu_request_t request;
    gpu_grant_t grant;
    uint8_t vm_id = 0;
    int ret;
    
    printf("=== SEV GPU Manager Test Application ===\n\n");
    
    /* Connect to manager */
    printf("[*] Connecting to GPU manager...\n");
    ret = sev_gpu_client_open(&client, vm_id, "TestVM");
    if (ret < 0) {
        fprintf(stderr, "[-] Failed to connect to manager\n");
        return 1;
    }
    
    printf("[+] Connected to manager\n");
    printf("    Shared memory size: %zu bytes\n", client.shmem_size);
    printf("    Header magic: 0x%llx\n", client.header->magic);
    printf("    Version: %u\n", client.header->version);
    printf("    Number of VMs: %u\n\n", client.header->num_vms);
    
    /* Request GPU */
    printf("[*] Requesting GPU access...\n");
    ret = sev_gpu_client_request_gpu(&client, 1000000, 128);  /* 1 second, mid priority */
    if (ret < 0) {
        fprintf(stderr, "[-] Failed to request GPU\n");
        goto cleanup;
    }
    
    /* Wait for grant */
    printf("[*] Waiting for GPU grant (max 5 seconds)...\n");
    ret = sev_gpu_client_wait_grant(&client, &grant, 5000);
    if (ret < 0) {
        fprintf(stderr, "[-] GPU grant timeout or error\n");
        goto cleanup;
    }
    
    printf("[+] GPU grant received!\n");
    printf("    VM ID: %u\n", grant.vm_id);
    printf("    Status: %u\n", grant.status);
    printf("    Allocated: %u us\n", grant.allocated_us);
    printf("    Start time: %llu ns\n", grant.grant_start_ns);
    printf("    End time: %llu ns\n\n", grant.grant_end_ns);
    
    /* Simulate GPU work */
    printf("[*] Running GPU workload...\n");
    printf("    (In real code, this would be CUDA kernel launches)\n");
    sleep(1);
    printf("[+] Workload complete\n\n");
    
    /* Release GPU */
    printf("[*] Releasing GPU...\n");
    ret = sev_gpu_client_release_gpu(&client);
    if (ret < 0) {
        fprintf(stderr, "[-] Failed to release GPU\n");
        goto cleanup;
    }
    
    printf("[+] GPU released\n");
    
cleanup:
    sev_gpu_client_close(&client);
    printf("\n[*] Disconnected from manager\n");
    return ret < 0 ? 1 : 0;
}
