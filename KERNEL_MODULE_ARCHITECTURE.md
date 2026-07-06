# SEV GPU Manager - Kernel Module Architecture

**Status**: Initial Implementation  
**Date**: 2026-06-21  

## Overview

The SEV GPU Manager is a **kernel module-based solution** for managing GPU access between multiple SEV-isolated VMs. Unlike user-space approaches, kernel-level notification handling provides:

- ✅ **Low latency**: Notifications caught at OS level, not forwarded to user-space
- ✅ **Efficient scheduling**: Kernel can make decisions without context switches
- ✅ **Direct control**: Kernel module owns shared memory, VMs access via mmap
- ✅ **SEV-native**: Works with non-private memory model

---

## Architecture

### Three-Layer Model

```
┌─────────────────────────────────────────────┐
│ User-space Applications                     │
│  ├─ Orchestrator VM (trains GPU models)     │
│  ├─ Data Holder VM (serves datasets)        │
│  └─ Other VMs requesting GPU time           │
│                                             │
│  Uses: sev_gpu_client.h library             │
└────────────────────┬────────────────────────┘
                     │ ioctl() syscalls
                     │ mmap() for shared memory
                     ↓
┌─────────────────────────────────────────────┐
│ Kernel Module: sev_gpu_manager              │
│ (/dev/sev_gpu_manager)                      │
│                                             │
│  ├─ Shared memory allocator                 │
│  ├─ Request/Grant routing                   │
│  ├─ Scheduling decisions                    │
│  ├─ eventfd notifications                   │
│  └─ No user-space forwarding                │
└────────────────────┬────────────────────────┘
                     │ mmap (to non-private pages)
                     │ eventfd (for IPC)
                     ↓
┌─────────────────────────────────────────────┐
│ Shared Memory Region (Non-Private for SEV)  │
│ ┌────────────────────────────────────────┐  │
│ │ Header (1 page)                        │  │
│ ├────────────────────────────────────────┤  │
│ │ Request Channel (64 MB)                │  │
│ │ VMs write GPU requests here            │  │
│ ├────────────────────────────────────────┤  │
│ │ Grant Channel (64 MB)                  │  │
│ │ Kernel writes GPU grants here          │  │
│ ├────────────────────────────────────────┤  │
│ │ Data Buffers (256 MB × MAX_VMS)        │  │
│ │ Per-VM data regions for communication  │  │
│ └────────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

---

## File Structure

```
sev-changes/
├── kernel/
│   ├── sev_gpu_manager.h         (~300 lines - data structures, ioctl defs)
│   ├── sev_gpu_manager.c         (~700 lines - kernel module implementation)
│   └── Makefile                  (build configuration)
│
└── userspace/
    ├── sev_gpu_client.h          (~100 lines - user-space API)
    ├── sev_gpu_client.c          (~300 lines - client implementation)
    └── example_vm_app.c          (~100 lines - example usage)
```

---

## How It Works

### Phase 1: Initialization

```
1. Kernel module loads
   └─ sev_gpu_init() called
      ├─ allocate_shared_memory() - allocates non-private pages
      ├─ Initialize manager state
      ├─ Register character device (/dev/sev_gpu_manager)
      └─ Export mmap interface

2. VM application starts
   └─ sev_gpu_client_open() called
      ├─ open("/dev/sev_gpu_manager")
      ├─ ioctl(REGISTER_VM)
      └─ mmap() shared memory
```

### Phase 2: GPU Request

```
3. VM requests GPU
   └─ sev_gpu_client_request_gpu(duration=1000000, priority=128)
      ├─ ioctl(REQUEST_GPU)
      └─ Kernel logs request
      
4. Kernel processes request
   └─ Kernel scheduling logic
      ├─ Check if GPU available
      ├─ Make scheduling decision
      └─ Write grant to shared memory
      
5. VM polls for grant
   └─ sev_gpu_client_wait_grant()
      ├─ Read grant_channel in shared memory
      ├─ Check if status == GRANTED
      └─ Return grant to application
      
6. VM runs GPU workload
   └─ Application uses VFIO or kernel driver
      ├─ Launch CUDA kernels
      ├─ Memory transfers
      └─ GPU computation
      
7. VM releases GPU
   └─ sev_gpu_client_release_gpu()
      └─ ioctl(RELEASE_GPU)
```

---

## Kernel Module Responsibilities

### 1. Shared Memory Management
- Allocates large contiguous non-private region for SEV
- Exposes via mmap() for user-space VMs
- Manages metadata in header page
- Pre-allocates per-VM data buffers

### 2. VM Registration
- Accepts VM registration via ioctl
- Tracks VM IDs, PIDs, priorities
- Stores eventfd contexts for notifications

### 3. Request Processing
- Accepts GPU requests from VMs
- Maintains request queue
- Implements scheduling policy (round-robin, priority, fairness)

### 4. Grant Decision
- Determines which VM gets GPU next
- Writes grant to shared memory
- Signals VM via eventfd (no user-space forwarding)

### 5. Notification Handling
- Receives notifications at kernel level
- eventfd signals directly to VMs (kernel-to-kernel, no detours)
- No user-space daemon needed

---

## Data Structures

### Request Channel (VM → Kernel)
```c
typedef struct {
    uint8_t vm_id;              // Which VM is requesting
    uint8_t priority;           // Priority (higher = more urgent)
    uint16_t msg_type;          // GPU_REQ_TIME, etc.
    uint32_t duration_us;       // How long GPU needed
    uint32_t required_vram_mb;  // VRAM requirements
    uint64_t timestamp_ns;      // Request timestamp
} gpu_request_t;
```

### Grant Channel (Kernel → VM)
```c
typedef struct {
    uint8_t vm_id;              // Which VM gets GPU
    uint8_t status;             // GRANTED, DENIED, QUEUED
    uint32_t allocated_us;      // Actual time granted
    uint64_t grant_start_ns;    // When to start
    uint64_t grant_end_ns;      // When to stop
} gpu_grant_t;
```

### Shared Memory Header
```c
typedef struct {
    uint64_t magic;             // Validation: 0xDEADBEEFCAFEBABE
    uint32_t version;           // Protocol version
    uint32_t num_vms;           // Number of registered VMs
    
    /* Region offsets and sizes */
    uint64_t request_region_off;
    uint64_t grant_region_off;
    uint64_t data_region_off;
    
    /* Per-VM data buffer addresses */
    uint64_t vm_data_offset[MAX_VMS];
    uint32_t vm_data_size[MAX_VMS];
} sev_gpu_shmem_header_t;
```

---

## ioctl Commands

### SEV_GPU_IOC_REGISTER_VM
```c
// Input: sev_gpu_ioctl_register_vm_t
// Purpose: Register VM with manager
ret = ioctl(fd, SEV_GPU_IOC_REGISTER_VM, &reg);
```

### SEV_GPU_IOC_GET_SHMEM
```c
// Output: sev_gpu_ioctl_get_shmem_t
// Purpose: Get shared memory physical address and size
ret = ioctl(fd, SEV_GPU_IOC_GET_SHMEM, &shmem_info);
```

### SEV_GPU_IOC_REQUEST_GPU
```c
// Input: sev_gpu_ioctl_request_gpu_t
// Purpose: Request GPU access
ret = ioctl(fd, SEV_GPU_IOC_REQUEST_GPU, &req);
```

### SEV_GPU_IOC_RELEASE_GPU
```c
// Input: sev_gpu_ioctl_release_gpu_t
// Purpose: Release GPU
ret = ioctl(fd, SEV_GPU_IOC_RELEASE_GPU, &rel);
```

---

## Building

### Prerequisites
```bash
# Install kernel headers
sudo apt-get install linux-headers-$(uname -r)

# Install build tools
sudo apt-get install build-essential
```

### Build Kernel Module
```bash
cd sev-changes/kernel
make

# Should produce: sev_gpu_manager.ko
```

### Build User-space Library & Example
```bash
cd sev-changes/userspace

# Compile client library
gcc -c -fPIC sev_gpu_client.c -I../kernel -o sev_gpu_client.o

# Create shared library
gcc -shared -o libsev_gpu.so sev_gpu_client.o

# Compile example
gcc -o example_vm_app example_vm_app.c sev_gpu_client.c -I../kernel
```

---

## Installation & Usage

### 1. Load Kernel Module
```bash
cd sev-changes/kernel
make

# Insert module
sudo insmod sev_gpu_manager.ko

# Verify it loaded
lsmod | grep sev_gpu
dmesg | tail -20

# Check device file
ls -la /dev/sev_gpu_manager
```

### 2. Run Example Application
```bash
cd sev-changes/userspace
./example_vm_app

# Output should show:
# [+] Connected to manager
# [+] GPU requested for 1000000 us (priority 128)
# [+] GPU granted!
# [+] GPU released
```

### 3. Monitor Kernel Logs
```bash
# In another terminal
dmesg -w

# Watch for module messages:
# sev_gpu: module loading
# sev_gpu: shared memory allocated
# sev_gpu: device opened
# sev_gpu: GPU request from VM0
```

---

## Kernel Module Notifications (eventfd)

### Current Implementation (Basic)
The kernel module accepts ioctl calls and writes grants to shared memory. VMs poll the grant channel.

### Future Enhancement (eventfd-Based)
For true async notification without polling:

```c
// Kernel module stores eventfd contexts per VM
struct eventfd_ctx *request_notifier;  // Signals when VM has pending request
struct eventfd_ctx *grant_notifier;    // Signals when grant available

// When VM submits request
eventfd_signal(request_notifier, 1);  // Wake kernel scheduler

// When kernel makes grant decision
eventfd_signal(grant_notifier, 1);    // Wake VM from wait
```

This eliminates polling entirely - notifications go directly from kernel to kernel.

---

## Key Advantages Over User-Space

| Feature | User-Space Daemon | Kernel Module |
|---------|------------------|---------------|
| **Latency** | ~100 μs (context switch) | <10 μs (no switch) |
| **Notification** | Forwarded to daemon | Caught directly |
| **Memory management** | Malloc overhead | Direct kernel allocation |
| **Scheduling** | App-level decisions | Kernel-level (atomic) |
| **Scalability** | Limited (daemon bottleneck) | Good (concurrent handling) |
| **Complexity** | Simple | Moderate |
| **SEV native** | Possible | Better (direct shared mem control) |

---

## Performance Targets

| Metric | Target | Method |
|--------|--------|--------|
| **Request latency** | <100 μs | Shared memory write |
| **Grant latency** | <1 ms | Kernel scheduling + eventfd |
| **Context switch** | <10 ms | Existing VFIO (no change) |
| **Throughput** | >1 Gbps | Direct shared memory access |
| **Fairness** | <2x ratio | Round-robin scheduling |

---

## Next Steps

### Phase 1 (Current)
- [x] Kernel module skeleton with shared memory
- [x] User-space client library
- [x] Basic ioctl interface
- [x] Example application

### Phase 2 (TODO)
- [ ] Implement round-robin scheduler in kernel
- [ ] Add priority queue support
- [ ] Implement eventfd-based notifications
- [ ] Add fairness enforcement (aging)
- [ ] Stress testing with multiple VMs

### Phase 3 (TODO)
- [ ] Optimize for multi-VM workloads
- [ ] NUMA awareness
- [ ] Lock-free data structures
- [ ] Performance profiling

### Phase 4 (TODO)
- [ ] Integration with GPU driver (VFIO)
- [ ] Context switching hooks
- [ ] SEV-specific optimizations

---

## Testing

### Single VM Test
```bash
./example_vm_app
# Should show successful GPU request and grant
```

### Multi-VM Simulation (Future)
```bash
# Run multiple instances (simulating different VMs)
./example_vm_app &
./example_vm_app &
./example_vm_app &

# Monitor kernel logs for scheduling
dmesg -w | grep sev_gpu
```

---

## Debugging

### Module Load Failures
```bash
dmesg | tail -50  # Check for error messages
insmod sev_gpu_manager.ko  # Try with verbose output
```

### mmap Issues
```bash
strace ./example_vm_app  # Trace system calls
# Look for EACCES, ENOMEM, etc.
```

### Shared Memory Problems
```bash
cat /proc/meminfo  # Check available memory
lsof /dev/sev_gpu_manager  # See who has device open
```

---

## SEV-Specific Considerations

### Non-Private Memory
- Kernel module allocates pages in non-private region
- All VMs can mmap and access these pages
- hypervisor marks pages as shared (not encrypted)
- SSL encryption protects sensitive data at application level

### IOMMU & Device Isolation
- VFIO handles GPU device isolation
- This module handles IPC/scheduling
- Keep them separate (no coupling)

### Notification Performance
- eventfd notifications go kernel-to-kernel
- No hypervisor intervention needed
- Low latency even under SEV

---

## References

- Linux LDD3: Device File Operations (mmap, ioctl)
- Linux eventfd(2) man page
- SEV architecture (AMD docs)
- VFIO documentation (device access)

