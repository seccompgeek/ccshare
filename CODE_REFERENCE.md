# Communication Channel Code Reference

## Quick File Locations

### Core Communication Files
| Component | Files | Purpose |
|-----------|-------|---------|
| **SSL Server** | `tgpu-llm.c/ssl/server.c` | Data holder - listens for orchestrator requests |
| **SSL Client** | `tgpu-llm.c/ssl/client.c` | Orchestrator - connects to data holder |
| **Message Protocol** | `tgpu-llm.c/ssl/msg_def.h` | Defines all message types and structures |
| **Server Header** | `tgpu-llm.c/ssl/server.h` | Server function declarations |
| **Client Header** | `tgpu-llm.c/ssl/client.h` | Client function declarations |
| **Cert Generation** | `tgpu-llm.c/ssl/certs_gen.sh` | Script to generate TLS certificates |

### Application Layer
| Component | Files | Purpose |
|-----------|-------|---------|
| **Data Holder** | `tgpu-llm.c/data_holder.c` | Data holder VM executable (runs server) |
| **Data Loader** | `tgpu-llm.c/remote_dataloader.h` | Orchestrator-side data loading interface |
| **Key I/O** | `tgpu-llm.c/key_io.h`, `key_io_lib.c` | Encryption key management |
| **Training** | `tgpu-llm.c/train_gpt2_fp32.cu` | Orchestrator-side training (uses remote_dataloader) |

### Internal Headers
| Component | Files | Purpose |
|-----------|-------|---------|
| **Config** | `tgpu-llm.c/internal.h` | Internal constants and definitions |

---

## Key Functions

### Server (Data Holder)

**Initialization**:
- `get_server_context()` - Creates OpenSSL context (server.c:30)
- `get_socket()` - Creates listening socket (server.c:81)
- `server()` - Main server loop (server.c:148)

**Request Handling**:
- `remote_transfer()` - Handles data chunk requests (server.c:95)
- Message handler switch: type checking at server.c:280

### Client (Orchestrator)

**Initialization**:
- `get_client_context()` - Creates OpenSSL context (client.c:16)
- `ssl_client_init()` - Initializes SSL client (client.c:69)

**Communication**:
- `client_send()` - Generic SSL send/receive (client.c:81)
- `client_initialise_remote_dataset()` - Sends dataset info request
- `client_dataset_transfer_chunk()` - Sends data chunk request
- `client_dataset_transfer_chunk_with_time()` - Data chunk request with timing

### External Dependencies

Called by communication code but defined elsewhere:
- `load_dataset()` - Loads dataset into data holder memory
- `tgpu_transfer_buffer()` - Performs actual data transfer
- `tgpu_transfer_buffer_with_time()` - Data transfer with timing
- `tgpu_key_io_size()` - Returns size of key material

---

## Port Configuration

**Default Port**: 11451 (defined in `data_holder.c` line 34)

```c
#define PORT_NUM 11451
```

Used in:
- Server: `server()` function parameter (server.c:148)
- Client: `ssl_client_init()` connection string

Example connection string: `"localhost:11451"` or `"<data-holder-ip>:11451"`

---

## Buffer Sizes

| Buffer | Size | Usage |
|--------|------|-------|
| `BUFSIZE` | 4 MB | SSL message buffer (client.c:18, server.c:27) |
| `SSL_TRANSFER_SIZE` | 16 KB | Per-record SSL chunk size (client.c:20, server.c:29) |

These determine throughput chunking for large transfers.

---

## Message Type Constants

```c
#define MSG_REQ_DATASET_INFO    0   // Orchestrator -> Data Holder
#define MSG_RSP_DATASET_INFO    1   // Data Holder -> Orchestrator
#define MSG_REQ_DATASET_CHUNK   2   // Orchestrator -> Data Holder
#define MSG_RSP_DATASET_CHUNK   3   // Data Holder -> Orchestrator
#define MSG_RSP_ERROR           255 // Any -> Any (error response)
```

---

## Execution Flow

### Data Holder VM Boot
```bash
# From README_AEC.md
sudo ./data_holder dev/data/edu/edu.bin
```

This:
1. Initializes OpenSSL context
2. Creates listening socket on port 11451
3. Enters `server()` main loop
4. Waits for incoming client connections

### Orchestrator VM Boot
```bash
# From launch_vm.sh or orchestrator script
```

Then training code:
1. Calls `remote_dataloader_init()`
2. Which calls `client_initialise_remote_dataset()`
3. Which calls `client_send()` with `MSG_REQ_DATASET_INFO`
4. Enters training loop
5. Repeatedly calls `remote_dataloader_transfer()`
6. Which calls `client_dataset_transfer_chunk()`

---

## SEV Compatibility Checklist

- [ ] **SSL/TLS Communication**: Should work as-is (no TDX-specific code)
- [ ] **Certificate Exchange**: Requires valid certs in both VMs
- [ ] **Physical Address Passing**: Need to verify SEV guest can extract valid paddr
- [ ] **Direct Memory Access**: Need to verify orchestrator→data_holder writes work
- [ ] **Port Accessibility**: Ensure network connectivity between VMs
- [ ] **Certificate Paths**: Update paths if different in SEV setup
- [ ] **Connection String**: Verify data holder IP/hostname for client connection

---

## Known Requirements from TDX Code

From code inspection, the data transfer assumes:
1. Valid physical addresses can be obtained (key_io interface)
2. One VM can write to another VM's physical memory
3. Encryption key material can be exchanged safely
4. Time measurements are available (for profiling)

These requirements may need SEV-specific implementations.
