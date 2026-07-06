# TGPU Communication Channels

## Overview

The TGPU system uses **TLS/SSL-based communication** between two VMs:
1. **Data Holder VM**: Manages the dataset and serves data chunks
2. **Orchestrator VM**: Runs the training loop and requests data

The communication is secured using **X.509 certificates** and follows a **request-response message protocol**.

---

## Communication Architecture

### Transport Layer
- **Protocol**: TCP/IP over network (virtio-net in the TDX case)
- **Security**: TLS 1.2/1.3 with mutual certificate authentication
- **Port**: 11451 (configurable, see `data_holder.c` line 34)
- **Buffer Size**: 4 MB (BUFSIZE in client.c and server.c)
- **SSL Transfer Chunk**: 16 KB per record

### TLS/SSL Implementation
- Uses OpenSSL library
- **Server** (data_holder.c): Listening on port 11451
- **Client** (train_gpt2_fp32.cu / remote_dataloader.h): Connects to data holder
- **Certificates**: Mutual TLS authentication (CA cert, server cert/key, client cert/key)
  - Located in: `ssl/` directory with `certs_gen.sh` for generation

---

## Message Protocol

### Message Structure
```c
typedef struct {
    uint8_t type;      // Message type (0-3 or 255)
    uint32_t size;     // Size of payload (buf)
    char buf[];        // Variable-length payload
} tgpu_msg_t;
```

### Message Types

#### 1. Dataset Info Exchange
```
CLIENT -> SERVER: MSG_REQ_DATASET_INFO (type=0)
├─ Batch size (B)
├─ Token length (T)
├─ Buffer size for transfers
├─ Ring buffer flag (1 = continuous looping, 0 = once)
└─ Report timing flag (1 = include performance stats)

SERVER -> CLIENT: MSG_RSP_DATASET_INFO (type=1)
├─ Number of samples in dataset
└─ Time statistics from previous session
    ├─ grab_gpu: Time to grab GPU
    ├─ import_key: Time to import encryption key
    ├─ buffer_read: Time to read from buffer
    ├─ gpu_write: Time to write to GPU
    └─ export_key: Time to export encryption key
```

**Location**: `ssl/msg_def.h` lines 13-23

#### 2. Dataset Chunk Transfer
```
CLIENT -> SERVER: MSG_REQ_DATASET_CHUNK (type=2)
├─ paddr_inputs: Physical address of input tensor in orchestrator VM
├─ paddr_targets: Physical address of target tensor in orchestrator VM
├─ key_size: Size of encryption key
└─ key: Encryption key data

SERVER -> CLIENT: MSG_RSP_DATASET_CHUNK (type=3)
├─ ret_val: Return value (negative = error, positive = num samples transferred)
├─ total_time: Total transfer time (if timing enabled)
├─ key_size: Size of encryption key
└─ key: Return encryption key data
```

**Location**: `ssl/msg_def.h` lines 25-31

#### 3. Error Response
```
type=255: MSG_RSP_ERROR
└─ buf: Error message string (up to 255 bytes)
```

---

## Communication Flow

### Initialization Phase
1. **Orchestrator** calls `client_initialise_remote_dataset()` (remote_dataloader.h:60)
2. **Client** opens SSL connection to data holder on port 11451
3. **Client** performs TLS handshake with mutual certificate verification
4. **Client** sends `MSG_REQ_DATASET_INFO`
5. **Server** loads dataset and responds with `MSG_RSP_DATASET_INFO`

**Key Functions**:
- Server: `get_server_context()`, `get_socket()` (server.c)
- Client: `get_client_context()`, `ssl_client_init()` (client.c)

### Data Transfer Phase (Repeated per batch)
1. **Orchestrator** calls `client_dataset_transfer_chunk()` (remote_dataloader.h:85)
2. **Client** sends `MSG_REQ_DATASET_CHUNK` with:
   - Physical addresses of input/target buffers in orchestrator VM
   - Encryption key to use for this transfer
3. **Server** receives request in `remote_transfer()` (server.c:95)
4. **Server** calls `tgpu_transfer_buffer()` which:
   - Reads encrypted key material
   - Accesses data holder's local dataset
   - Writes data directly to orchestrator's physical memory (DMA-like)
   - Processes and returns new key
5. **Server** sends `MSG_RSP_DATASET_CHUNK` with:
   - Number of samples transferred
   - Updated encryption key
6. **Client** receives response and returns control

**Key Functions**:
- Client: `client_send()`, `client_dataset_transfer_chunk()` (client.c)
- Server: `remote_transfer()`, message handling loop (server.c:280+)

---

## Certificate Management

### Certificate Generation
- Script: `ssl/certs_gen.sh`
- Creates:
  - CA certificate and key
  - Server certificate and key (for data holder)
  - Client certificate and key (for orchestrator)

### Certificate Verification
- **Server verification**: Verifies client certificate signed by CA
- **Client verification**: Verifies server certificate signed by CA
- **Depth**: 1 (only direct CA signature required)

**Code**:
- Server: `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL)` (server.c:64)
- Client: `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL)` (client.c:63)

---

## Data Transfer Details

### Key Material Exchange
The actual data transfer involves encryption key exchange:
1. **Orchestrator** sends a key to data holder
2. **Data holder** uses this key to read encrypted dataset
3. **Data holder** writes decrypted data to orchestrator's physical memory
4. **Data holder** derives/exports a new key back to orchestrator

**Functions**:
- `tgpu_transfer_buffer()` (external, implemented in data_holder.c)
- `tgpu_transfer_buffer_with_time()` (timing version)

### Memory Access
- **Orchestrator** passes physical addresses of its memory regions
- **Data holder** writes data directly using these physical addresses
- This implies **shared memory** or **direct DMA-like access** between VMs

**Code**: server.c lines 100-112 request handling

---

## Performance Monitoring

### Timing Statistics
When `report_time=1`, server includes timing breakdown:
```c
typedef struct {
    double grab_gpu;     // Time spent acquiring GPU resources
    double import_key;   // Time spent importing encryption key
    double buffer_read;  // Time spent reading from local buffer
    double gpu_write;    // Time spent writing to GPU
    double export_key;   // Time spent exporting new key
} time_statistics_t;
```

**Location**: ssl/msg_def.h lines 17-22

---

## File Organization

### Client-side (Orchestrator VM)
- **Main training file**: `train_gpt2_fp32.cu`
- **Data loader interface**: `remote_dataloader.h`
- **SSL client**: `ssl/client.c`, `ssl/client.h`
- **Message definitions**: `ssl/msg_def.h`

### Server-side (Data Holder VM)
- **Data holder server**: `data_holder.c`
- **SSL server**: `ssl/server.c`, `ssl/server.h`
- **Encryption key I/O**: `key_io.h`, `key_io_lib.c`
- **Message definitions**: `ssl/msg_def.h`

### Shared
- **Certificate storage**: `ssl/` directory
- **Internal header**: `internal.h`

---

## Key Observations for SEV Adaptation

1. **No TDX-specific communication**: The SSL/message protocol is TDX-agnostic
2. **Physical address exchange**: Relies on `paddr_inputs` and `paddr_targets` being valid in target VM
3. **Direct memory access**: Data holder must write directly to orchestrator's memory
4. **Certificate-based authentication**: More secure than TDX implicit trust
5. **Ring buffer support**: Handles both one-pass and continuous looping modes

---

## Next Steps

- [ ] Verify SEV guest supports physical address extraction (via UVM similar to TDX)
- [ ] Test SSL handshake between SEV-protected VMs
- [ ] Validate direct memory writes across SEV boundaries
- [ ] Create SEV-adapted key material exchange if needed
- [ ] Profile communication overhead under SEV
