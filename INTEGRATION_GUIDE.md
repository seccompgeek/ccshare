# Integration Guide: Adding Shared Memory Transport to SSL Code

## Overview

This guide shows how to integrate the shared memory + SSL transport into the existing `server.c` and `client.c` files. The key is that **SSL code changes minimally** - the transport layer is abstracted.

---

## Step 1: Update `server.h`

Add new function declarations for shared memory mode:

```c
/* Original socket-based functions */
int server(int port_num,
           const char *ca_pem,
           const char *cert_pem,
           const char *key_pem);

/* New shared memory-based functions */
int server_shared_mem(SharedMemBuffer *buffer,
                      const char *ca_pem,
                      const char *cert_pem,
                      const char *key_pem);

int server_shared_mem_with_context(SharedMemContext *ctx,
                                    const char *ca_pem,
                                    const char *cert_pem,
                                    const char *key_pem);
```

---

## Step 2: Update `server.c` - New Function

Add this new function **alongside** the existing `server()` function (don't replace it):

```c
#include "shared_mem_buffer.h"
#include "shared_mem_bio.h"

/*
 * Server implementation using shared memory transport
 * 
 * buffer: Pre-allocated shared memory buffer (created by caller)
 * ca_pem, cert_pem, key_pem: SSL certificate files (same as socket version)
 */
int server_shared_mem(SharedMemBuffer *buffer,
                      const char *ca_pem,
                      const char *cert_pem,
                      const char *key_pem) {
    if (!buffer) {
        printf("[-] server: buffer is NULL\n");
        return -1;
    }
    
    /* Initialize context (server=1, timeout=-1 for infinite) */
    SharedMemContext *ctx = shmem_context_init(buffer, 1, -1);
    if (!ctx) {
        printf("[-] server: failed to initialize shared memory context\n");
        return -1;
    }
    
    /* Proceed with SSL setup as before */
    return server_shared_mem_with_context(ctx, ca_pem, cert_pem, key_pem);
}

/*
 * Core server implementation - shared between socket and shared memory versions
 * 
 * Key difference: Uses BIO_new_shared_mem() instead of TCP socket BIO
 */
int server_shared_mem_with_context(SharedMemContext *ctx,
                                    const char *ca_pem,
                                    const char *cert_pem,
                                    const char *key_pem) {
    char *buffer;
    tgpu_msg_t *out_msg;
    size_t out_msg_size;
    tgpu_msg_t *recv_msg;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    BIO *bio;
    int rc, len, current_offset;
    int write_size;
    size_t expected_size;
    
    tgpu_msg_req_dataset_info_t *req_dataset_info;
    tgpu_msg_rsp_dataset_info_t *rsp_dataset_info;
    tgpu_msg_req_dataset_chunk_t *req_dataset_chunk;
    
    /* Initialize OpenSSL (same as socket version) */
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    
    /* Get SSL context (reuse existing function from socket version) */
    if (!(ssl_ctx = get_server_context(ca_pem, cert_pem, key_pem))) {
        printf("[-] server: failed to get server context\n");
        return -1;
    }
    
    /* Allocate message buffer */
    buffer = malloc(BUFSIZE);
    if (buffer == NULL) {
        printf("[-] server: failed to allocate recv buffer\n");
        SSL_CTX_free(ssl_ctx);
        return -1;
    }
    
    recv_msg = (tgpu_msg_t *)buffer;
    
    printf("[+] server: shared memory mode ready\n");
    
    /* Main loop - handle SSL connection over shared memory */
    while (1) {
        /* Create BIO for shared memory transport */
        bio = BIO_new_shared_mem(ctx);
        if (!bio) {
            printf("[-] server: failed to create shared memory BIO\n");
            break;
        }
        
        /* Get an SSL handle from the context */
        if (!(ssl = SSL_new(ssl_ctx))) {
            printf("[-] server: could not get an SSL handle from context\n");
            BIO_free(bio);
            break;
        }
        
        /* Associate BIO with SSL (shared memory transport) */
        SSL_set_bio(ssl, bio, bio);
        
        /* Perform SSL accept/handshake (blocking, will read from shared memory) */
        printf("[+] server: waiting for SSL handshake...\n");
        if ((rc = SSL_accept(ssl)) != 1) {
            printf("[-] server: SSL accept failed\n");
            SSL_shutdown(ssl);
            SSL_free(ssl);
            continue;
        }
        
        printf("[+] server: SSL handshake successful\n");
        
        /* Message receive loop (same protocol as socket version) */
        current_offset = 0;
        expected_size = 0;
        
        while (expected_size == 0 || current_offset < expected_size) {
            /* SSL_read blocks on BIO, which blocks on shared memory eventfd */
            len = SSL_read(ssl, buffer + current_offset, BUFSIZE - current_offset);
            
            if (len <= 0) {
                printf("[-] server: SSL read failed\n");
                break;
            }
            
            current_offset += len;
            
            if (current_offset > sizeof(tgpu_msg_t)) {
                if (expected_size == 0) {
                    expected_size = recv_msg->size + sizeof(tgpu_msg_t);
                }
            }
        }
        
        printf("[+] server: received %d bytes\n", current_offset);
        
        /* Message handling (identical to socket version) */
        out_msg_size = 0;
        out_msg = NULL;
        
        if (recv_msg->type == MSG_REQ_DATASET_INFO) {
            out_msg_size = sizeof(tgpu_msg_t) + sizeof(tgpu_msg_rsp_dataset_info_t);
            out_msg = malloc(out_msg_size);
            memset(out_msg, 0, out_msg_size);
            out_msg->type = MSG_RSP_DATASET_INFO;
            out_msg->size = sizeof(tgpu_msg_rsp_dataset_info_t);
            
            req_dataset_info = (tgpu_msg_req_dataset_info_t *)recv_msg->buf;
            rsp_dataset_info = (tgpu_msg_rsp_dataset_info_t *)out_msg->buf;
            
            rsp_dataset_info->num_samples = load_dataset(
                req_dataset_info->B, req_dataset_info->T, 
                req_dataset_info->size, req_dataset_info->ring_buffer, 
                &rsp_dataset_info->time_statistics);
            
            printf("[+] server: handled dataset info\n");
        }
        else if (recv_msg->type == MSG_REQ_DATASET_CHUNK) {
            req_dataset_chunk = (tgpu_msg_req_dataset_chunk_t *)recv_msg->buf;
            remote_transfer(req_dataset_chunk, &out_msg, &out_msg_size);
            printf("[+] server: handled data chunk\n");
        }
        
        /* Send response */
        if (out_msg) {
            current_offset = 0;
            while (current_offset < out_msg_size) {
                len = SSL_write(ssl, ((void *)out_msg) + current_offset, 
                               out_msg_size - current_offset);
                if (len <= 0) {
                    printf("[-] server: SSL write failed\n");
                    break;
                }
                current_offset += len;
            }
            free(out_msg);
        }
        
        /* Cleanup this connection */
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    
    /* Cleanup */
    free(buffer);
    SSL_CTX_free(ssl_ctx);
    shmem_context_cleanup(ctx);
    BIO_meth_free_shared_mem();
    
    return 0;
}
```

---

## Step 3: Update `client.h`

Add new function declarations:

```c
/* Original socket-based functions */
int ssl_client_init(const char *l_conn_str, const char *ca_pem,
                    const char *cert_pem, const char *key_pem);

/* New shared memory-based functions */
int ssl_client_init_shared_mem(SharedMemContext *ctx,
                               const char *ca_pem,
                               const char *cert_pem,
                               const char *key_pem);

int client_initialise_remote_dataset_shared_mem(SharedMemContext *ctx,
                                                int B, int T, size_t size, 
                                                int *num_samples_p, 
                                                int ring_buffer, int report_time);

int client_dataset_transfer_chunk_shared_mem(SharedMemContext *ctx,
                                            uint64_t paddr_r_inputs, 
                                            uint64_t paddr_r_targets);
```

---

## Step 4: Update `client.c` - Connection Layer

Replace the TCP connection code with shared memory. In `client_send()`, instead of:

```c
/* OLD: TCP socket connection */
if (!(sbio = BIO_new_ssl_connect(ctx))) {
    fprintf(stderr, "[-] client: could not get a BIO object\n");
    goto fail1;
}

BIO_get_ssl(sbio, &ssl);

if (BIO_set_conn_hostname(sbio, conn_str) != 1) {
    fprintf(stderr, "[-] client: could not connect to server\n");
    goto fail2;
}
```

Add new version for shared memory:

```c
/* NEW: Shared memory BIO connection */
int client_send_shared_mem(SharedMemContext *shmem_ctx, 
                           void *buffer, size_t size, 
                           void *out_buffer, size_t *out_size) {
    SSL *ssl;
    BIO *bio;
    int rc = -1;
    int len;
    int current_offset;
    void *staging_buffer;
    
    /* Initialize OpenSSL if not already done */
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    
    /* Get a static SSL context (set up once via ssl_client_init) */
    /* This is stored globally and reused for all connections */
    extern SSL_CTX *g_ssl_client_ctx;
    
    /* Create shared memory BIO */
    bio = BIO_new_shared_mem(shmem_ctx);
    if (!bio) {
        fprintf(stderr, "[-] client: could not create shared memory BIO\n");
        goto fail1;
    }
    
    /* Get SSL handle from context */
    ssl = SSL_new(g_ssl_client_ctx);
    if (!ssl) {
        fprintf(stderr, "[-] client: could not get SSL handle\n");
        goto fail2;
    }
    
    /* Associate BIO with SSL */
    SSL_set_bio(ssl, bio, bio);
    
    /* Perform SSL connect/handshake */
    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "[-] client: SSL connect failed\n");
        goto fail3;
    }
    
    printf("[+] client: SSL handshake successful\n");
    
    /* Write request (blocks until written) */
    current_offset = 0;
    while (current_offset < size) {
        len = SSL_write(ssl, ((void *)buffer) + current_offset, 
                       size - current_offset);
        if (len <= 0) {
            printf("[-] client: SSL_write failed\n");
            goto fail3;
        }
        current_offset += len;
    }
    
    printf("[+] client: sent %d bytes\n", current_offset);
    
    /* Read response (blocks until data available) */
    staging_buffer = malloc(BUFSIZE);
    if (!staging_buffer) {
        printf("[-] client: staging buffer allocation failed\n");
        goto fail3;
    }
    
    current_offset = 0;
    while ((len = SSL_read(ssl, staging_buffer, BUFSIZE)) > 0) {
        if (current_offset + len > *out_size) {
            printf("[-] client: result too large\n");
            goto fail4;
        }
        memcpy(out_buffer + current_offset, staging_buffer, len);
        current_offset += len;
    }
    
    *out_size = current_offset;
    printf("[+] client: received %d bytes\n", current_offset);
    
    rc = 0;
    
fail4:
    free(staging_buffer);
fail3:
    SSL_shutdown(ssl);
    SSL_free(ssl);
fail2:
    BIO_free(bio);
fail1:
    return rc;
}
```

---

## Step 5: Update `data_holder.c` Startup

Change from socket to shared memory:

```c
/* OLD: Start TCP server */
int main(int argc, char **argv) {
    // ...
    server(PORT_NUM, "ssl/ca.pem", "ssl/server.pem", "ssl/server-key.pem");
}

/* NEW: Start shared memory server */
int main(int argc, char **argv) {
    SharedMemBuffer *buffer = shmem_create();
    if (!buffer) {
        printf("[-] Failed to create shared memory\n");
        return 1;
    }
    
    /* TODO: Make this non-private (shared) with hypervisor */
    uint64_t paddr = shmem_get_paddr(buffer);
    printf("[+] Shared memory buffer at paddr=0x%lx\n", paddr);
    printf("[+] Pass this paddr to orchestrator VM\n");
    
    /* Start server on shared memory */
    return server_shared_mem(buffer, "ssl/ca.pem", "ssl/server.pem", "ssl/server-key.pem");
}
```

---

## Step 6: Update Training Code Startup

Change from socket to shared memory:

```c
/* In train_gpt2_fp32.cu or wherever data loader is initialized */

/* OLD: TCP client */
remote_dataloader_init(loader, B, T, buffer_size, paddr_inputs, paddr_targets, 
                      uvm_fd, ring_buffer, report_time);
// This internally calls:
// client_initialise_remote_dataset(B, T, buffer_size, &num_samples, ...)

/* NEW: Shared memory client */
// Receive paddr from data holder (via shared config or environment)
uint64_t shmem_paddr = ...; // Get from hypervisor or config

SharedMemBuffer *buffer = shmem_map(shmem_paddr);
if (!buffer) {
    printf("[-] Failed to map shared memory\n");
    return;
}

SharedMemContext *ctx = shmem_context_init(buffer, 0, -1); // is_server=0
if (!ctx) {
    printf("[-] Failed to initialize context\n");
    return;
}

// Initialize SSL over shared memory
int ret = ssl_client_init_shared_mem(ctx, "ssl/ca.pem", "ssl/client.pem", "ssl/client-key.pem");
if (ret < 0) {
    printf("[-] Failed to initialize SSL\n");
    return;
}

// Use shared memory-aware loader functions
remote_dataloader_init_shared_mem(loader, ctx, B, T, buffer_size, 
                                 paddr_inputs, paddr_targets, ring_buffer, report_time);
```

---

## Compilation

Update the `Makefile` to include new files:

```makefile
# Add to SSL object files
SSLOBJ = server.o client.o shared_mem_buffer.o shared_mem_bio.o

# Add include path if needed
CFLAGS += -I/usr/include/openssl

# Link flags
LDFLAGS += -lssl -lcrypto
```

---

## Testing Checklist

- [ ] Compile without errors
- [ ] Data holder starts and creates shared memory
- [ ] Orchestrator maps shared memory
- [ ] SSL handshake completes over shared memory
- [ ] Dataset info message exchanges correctly
- [ ] Data chunk transfer works
- [ ] Performance measurement (compare vs TCP)
- [ ] Run full training epoch to validate correctness

---

## Debugging Tips

1. **Check eventfd signals**: Add debug prints around `signal_eventfd()` calls
2. **Monitor buffer state**: Print ring buffer positions before/after transfers
3. **SSL errors**: Check `OpenSSL_add_ssl_algorithms()` and certificate loading
4. **Timeout issues**: Increase timeout_ms in context or set to -1 (infinite)
5. **Use strace**: `strace -e eventfd,read,write -f` to see I/O patterns

---

## Performance Expectations

- **TCP latency**: ~1-5ms per round-trip
- **Shared memory latency**: ~10-100μs per round-trip
- **Speedup factor**: 10-100x faster for communication phase
- **Total epoch time**: Depends on compute time vs communication ratio

If communication is 10% of epoch time, expect ~5-10% overall improvement.
If communication is 50% of epoch time, expect ~40-50% overall improvement.
