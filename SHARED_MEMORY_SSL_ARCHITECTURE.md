# Shared Memory + SSL Architecture for SEV

## Overview

Replace the TCP socket transport layer with a shared memory ring buffer, keeping the SSL application layer intact. Both VMs access the same physical memory region (non-private/non-encrypted by SEV), but SSL encrypts the message payloads.

```
┌─────────────────────────────────────────────────────┐
│ Current Architecture (TCP Sockets)                   │
├─────────────────────────────────────────────────────┤
│                                                       │
│  Application Layer (OpenSSL/SSL)                    │
│         ↓                                             │
│  Transport Layer (TCP sockets, BIO)                 │
│         ↓                                             │
│  Network (kernel TCP/IP stack)                      │
│         ↓                                             │
│  Physical Network (virtio-net)                      │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│ New Architecture (Shared Memory + SSL)              │
├─────────────────────────────────────────────────────┤
│                                                       │
│  Application Layer (OpenSSL/SSL) ← NO CHANGE       │
│         ↓                                             │
│  Transport Layer (Shared Mem BIO) ← NEW             │
│         ↓                                             │
│  Synchronization (eventfd/semaphore) ← NEW         │
│         ↓                                             │
│  Shared Memory Ring Buffer ← NEW                    │
│         ↓                                             │
│  Non-Private Guest Memory (SEV-ES visible)         │
└─────────────────────────────────────────────────────┘
```

---

## Key Components

### 1. Shared Memory Ring Buffer

**Structure**:
```c
typedef struct {
    // Write head (updated by writer)
    volatile uint32_t write_head;
    volatile uint32_t write_tail;
    
    // Read head (updated by reader)
    volatile uint32_t read_head;
    volatile uint32_t read_tail;
    
    // Synchronization
    int write_eventfd;  // Signaled when data available
    int read_eventfd;   // Signaled when space available
    
    // Ring buffer data
    uint8_t data[RING_BUFFER_SIZE];  // Typically 64 MB
} SharedMemRingBuffer;
```

**Memory Layout**:
```
┌─────────────────────────────────────┐
│ Metadata (header)                    │  ~64 bytes
├─────────────────────────────────────┤
│ Data Ring Buffer                     │  64 MB (configurable)
├─────────────────────────────────────┤
│ Padding for alignment                │
└─────────────────────────────────────┘
```

### 2. Custom OpenSSL BIO

**Purpose**: Replace TCP socket BIO with shared memory BIO

```c
BIO *BIO_new_shared_mem(SharedMemRingBuffer *buffer, int is_server);
```

**Implements**:
- `BIO_read()` - Read from ring buffer (with timeout via eventfd)
- `BIO_write()` - Write to ring buffer (with blocking if full)
- `BIO_ctrl()` - Control operations (flush, shutdown, etc.)

### 3. Synchronization

**EventFD for notifications**:
- Data holder VM: Writes to data, signals via write_eventfd
- Orchestrator VM: Reads eventfd, wakes up from `SSL_read()`
- Same in reverse for read_eventfd

**No busy waiting** - both VMs sleep on eventfd until signaled

### 4. Handshake Protocol

**Initial handshake** still uses SSL, but over shared memory transport:
1. Data holder: Creates shared memory, passes address to orchestrator (via cmdline/config)
2. Orchestrator: Maps shared memory region
3. Both: Initialize BIO with same buffer
4. SSL handshake proceeds normally (over shared memory instead of TCP)
5. Data starts flowing encrypted through shared memory

---

## Implementation Plan

### Phase 1: Core Ring Buffer
- [ ] `shared_mem_buffer.h` - Ring buffer data structure
- [ ] `shared_mem_buffer.c` - Allocation, mapping, synchronization

### Phase 2: Custom BIO
- [ ] `shared_mem_bio.c` - Custom OpenSSL BIO implementation
- [ ] Supports blocking reads/writes
- [ ] Eventfd-based wait

### Phase 3: Integration
- [ ] Modify `ssl/server.c` - Use shared memory BIO instead of TCP
- [ ] Modify `ssl/client.c` - Use shared memory BIO instead of TCP
- [ ] Update `ssl/server.h` and `ssl/client.h` - New init functions

### Phase 4: Startup Protocol
- [ ] Server: Create shared memory, listen on hypervisor channel for client request
- [ ] Client: Allocate/map shared memory, connect via hypervisor handshake
- [ ] Both: Exchange eventfd file descriptors

---

## File Changes Required

### New Files
```
tgpu-llm.c/ssl/
├── shared_mem_buffer.h
├── shared_mem_buffer.c
├── shared_mem_bio.c
└── shared_mem_handshake.h
```

### Modified Files
```
tgpu-llm.c/ssl/
├── server.c        (replace socket listen with shmem setup)
├── server.h        (add new init function)
├── client.c        (replace socket connect with shmem setup)
└── client.h        (add new init function)
```

### Minimal Changes
```
tgpu-llm.c/
├── data_holder.c   (1-2 lines for new init call)
└── train_gpt2_fp32.cu (1-2 lines for new init call)
```

---

## Benefits vs Sockets

| Aspect | TCP Sockets | Shared Mem + SSL |
|--------|-----------|-----------------|
| Latency | ~1-5ms per round-trip | ~10-100μs |
| Bandwidth | Limited by virtio-net | Full memory BW |
| Copies | 3-4 per direction | 1 per direction |
| CPU overhead | Kernel TCP/IP stack | Just SSL crypto |
| SEV compatible | ✅ Yes | ✅ Yes (better) |
| Complexity | Low | Medium |
| Debug tools | TCPdump, netstat | Custom tools |

**Performance Gain**: 10-100x lower latency for batch exchange = ~10-100ms savings per epoch in communication

---

## Memory Requirements

**Shared memory size**: Depends on transfer size
- Min: 128 MB (for 4MB message buffer + ring buffer)
- Recommended: 512 MB - 1 GB
- Per-batch overhead: Negligible (just pointer updates)

**Non-private memory**: Both VMs must allocate from same physical address space
- Use `sev-guest` hypercall or ballooning
- Mark as shared (non-private) during allocation
- Both VMs map at same or different virtual addresses (doesn't matter - uses paddr)

---

## Security Considerations

### What SEV provides
- VM isolation: Hypervisor can't read VM memory
- Attestation: VMs can verify encryption keys

### What SSL provides
- Authentication: Mutual cert verification (unchanged)
- Encryption: Message payloads encrypted with SSL key
- Integrity: HMAC/AEAD on all messages

### Combined Security
1. **Shared memory is plain**: Hypervisor might read it
2. **SSL encrypts it**: Only VMs with SSL key can decrypt
3. **Attestation**: VMs exchange certs to prove identity
4. **Result**: Secure even if hypervisor is compromised

---

## Migration Path

1. **Keep TCP code**: Don't delete, just add new functions
2. **Config flag**: Allow both modes (sockets vs shared memory)
3. **Test socket mode first**: Validate on SEV with TCP baseline
4. **Implement shared memory**: Parallel development
5. **Performance compare**: Measure both approaches
6. **Select winner**: Choose based on actual numbers

---

## Next Steps

1. Create `shared_mem_buffer.h/c` - Ring buffer primitives
2. Create `shared_mem_bio.c` - Custom OpenSSL BIO
3. Create startup handshake protocol
4. Integrate into server.c and client.c
5. Test with data holder + orchestrator on same SEV host
