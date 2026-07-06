# ivshmem vs Manual Shared Memory

## Quick Comparison

| Aspect | Manual (/dev/mem) | ivshmem |
|--------|-------------------|---------|
| **Setup Complexity** | High (paddr, non-private flags) | Low (QEMU config only) |
| **Inter-VM Coordination** | Manual (need to exchange paddr) | Automatic (ivshmem handles it) |
| **Non-Private Negotiation** | Manual (SEV guest driver calls) | Automatic (QEMU + hypervisor) |
| **Notification Mechanism** | Manual eventfd setup | Built-in ivshmem interrupts |
| **SEV Compatible** | ✅ Yes (but complex) | ✅ Yes (designed for it) |
| **TDX Compatible** | ✅ Yes | ✅ Yes |
| **Production Ready** | ⚠️ Requires kernel work | ✅ Yes (QEMU 4.0+) |

---

## ivshmem Overview

### What It Provides

1. **Memory Region Sharing**: OS-level abstraction for shared memory
2. **Notification Mechanism**: Inter-VM events via doorbell register
3. **Hot Plug Support**: Add/remove memory dynamically
4. **Multi-VM Support**: One region, multiple VMs access it
5. **Automatic Non-Private Handling**: QEMU handles SEV non-private setup

### How It Works

```
QEMU Configuration:
  -object memory-backend-file,id=mem0,size=64M,share=on,mem-path=/tmp/ivshmem
  -device ivshmem-plain,id=ivshmem0,memdev=mem0

Both VMs:
  ├── QEMU maps shared memory at virtual address
  ├── Both see same physical memory
  ├── Can communicate via read/write
  └── Use doorbell register for events

Result:
  VM1 writes → Shared memory → VM2 reads
  VM1 triggers doorbell → VM2 gets interrupt
```

---

## Implementation Strategy

### Option A: Use ivshmem Notifications (Recommended)

Replace our manual eventfd with ivshmem doorbell mechanism:

```c
// Instead of eventfd write
signal_eventfd(fd);

// Use ivshmem doorbell
ivshmem_notify(ivshmem_dev, peer_vm_id);
```

**Pros**:
- ✅ QEMU handles everything
- ✅ Cleaner inter-VM communication
- ✅ Lower overhead than eventfd
- ✅ Works with SEV transparently

**Cons**:
- Requires handling ivshmem device driver
- Notification is interrupt (not poll-based)

### Option B: Keep Ring Buffer, Use ivshmem for Memory Only

Keep our ring buffer design but use ivshmem instead of manual mapping:

```c
// Instead of shmem_create() + manual mmap
SharedMemBuffer *buffer = ivshmem_map();  // Maps automatically
```

**Pros**:
- ✅ Minimal code changes
- ✅ Simplifies startup
- ✅ All ring buffer logic unchanged

**Cons**:
- Still need eventfd (not much different)

### Option C: Use ivshmem Server + Notifications

Use ivshmem's server mode with doorbell for full coordination:

```c
Data Holder VM:
  └── Creates ivshmem region
  └── Waits for doorbell from orchestrator

Orchestrator VM:
  └── Maps ivshmem region
  └── Triggers doorbell when need data
  └── Waits for data-ready interrupt
```

**Pros**:
- ✅ Cleanest architecture
- ✅ Symmetric design
- ✅ QEMU does all coordination

**Cons**:
- Need to handle ivshmem doorbell interrupts

---

## Recommended Path: Option B (Hybrid)

Keep our ring buffer design but use ivshmem for memory allocation and mapping:

```
┌─────────────────────────────────────────────────┐
│           Our Ring Buffer Layer                  │
│  (shared_mem_buffer.c - no changes needed)     │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│        ivshmem Memory Management                │
│  (new: ivshmem_interface.h/c)                  │
│                                                  │
│  - ivshmem_create()  → creates region          │
│  - ivshmem_map()     → maps region              │
│  - ivshmem_notify()  → sends notification      │
│  - ivshmem_wait()    → waits for interrupt     │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│      QEMU ivshmem Device                        │
│  (transparent to our code)                     │
└─────────────────────────────────────────────────┘
```

---

## ivshmem Configuration

### In QEMU Launch Script

For data holder:
```bash
qemu-system-x86_64 \
    -object memory-backend-file,id=ivshmem_mem,\
            size=64M,\
            share=on,\
            mem-path=/tmp/sev-data-holder-shmem \
    -device ivshmem-plain,\
            id=ivshmem0,\
            memdev=ivshmem_mem,\
            bus=pcie.0 \
    ...
```

For orchestrator (same memory backend):
```bash
qemu-system-x86_64 \
    -object memory-backend-file,id=ivshmem_mem,\
            size=64M,\
            share=on,\
            mem-path=/tmp/sev-data-holder-shmem \
    -device ivshmem-plain,\
            id=ivshmem0,\
            memdev=ivshmem_mem,\
            bus=pcie.0 \
    ...
```

**Key Points**:
- Both VMs use **same** `mem-path`
- Size must match
- `share=on` enables sharing
- `ivshmem-plain` (not server mode) for simplicity

### Inside Guest VM

The shared memory appears at a fixed virtual address:
```
/sys/devices/pci*/*/resource2  ← The shared memory region
```

Or via libvfio-user if using ivshmem server mode.

---

## Implementation Changes

### File Changes Needed

**Remove/Simplify**:
- `shmem_create()` → Just ivshmem memory allocation
- `shmem_map()` → Just ivshmem mapping
- Manual paddr negotiation → Automatic (QEMU)
- Manual eventfd setup → ivshmem notifications

**Keep As-Is**:
- Ring buffer core (`shared_mem_buffer.c`)
- Custom BIO (`shared_mem_bio.c`)
- SSL layer (unchanged)

### New Files

```
ivshmem_interface.h     // High-level ivshmem API
ivshmem_interface.c     // Implementation
```

### Modified Files

```
shared_mem_buffer.h     // Minor tweaks to initialization
shared_mem_buffer.c     // Replace shmem_create/map with ivshmem calls
shared_mem_bio.c        // No changes needed
server.c                // Replace shmem_create() with ivshmem_create()
client.c                // Replace shmem_map() with ivshmem_map()
data_holder.c           // Use ivshmem instead of shmem
train_gpt2_fp32.cu      // Use ivshmem instead of shmem
```

---

## ivshmem Device Interface

### Linux Guest Kernel Access

```c
/* ivshmem appears as PCI device */
struct pci_dev *ivshmem_dev = pci_get_device(VENDOR_ID, DEVICE_ID, NULL);

/* Map the BAR region to access shared memory */
void *shmem_addr = ioremap(ivshmem_dev->resource[2].start, 64*1024*1024);

/* Doorbell address for notifications */
void *doorbell_addr = ioremap(ivshmem_dev->resource[0].start, PAGE_SIZE);

/* Write to doorbell to notify peer */
writel(1, doorbell_addr + 0xc);  // Offset 0xc = doorbell register
```

### Interrupt Handler

If using ivshmem server with peer notifications:
```c
static irqreturn_t ivshmem_interrupt_handler(int irq, void *dev_id) {
    /* Peer VM sent us a notification */
    /* Wake up any waiters */
    wake_up(&wait_queue);
    return IRQ_HANDLED;
}
```

---

## Advantages of ivshmem

### For Your Use Case
1. **Designed for SEV**: QEMU/hypervisor handles all non-private stuff
2. **No Boot Coordination**: QEMU sets it up before VMs start
3. **Simple Config**: Just add to launch script
4. **Proven**: Used in production for KVM clusters
5. **Cross-Platform**: Works on AMD EPYC, Intel Xeon, etc.

### For SEV Specifically
- Hypervisor can verify memory sharing policy
- Guests don't need to call SEV-specific hypercalls
- ivshmem automatically handles encryption boundaries
- No kernel patches required (QEMU + standard Linux is enough)

### For Performance
- Direct memory access (no copy)
- Doorbell interrupts (faster than polling)
- Minimal overhead

---

## Migration Path

### Step 1: Current State
```
Your Code:
  - shared_mem_buffer.h/c (manual memory management)
  - shared_mem_bio.c (custom BIO)
  - Manual eventfd sync
```

### Step 2: With ivshmem
```
Your Code:
  - Keep: shared_mem_bio.c (no changes)
  - Keep: Ring buffer logic (minimal changes)
  - Add: ivshmem_interface.h/c (new 200-line wrapper)
  - Replace: Manual memory/paddr/eventfd with ivshmem calls
  - Update: server.c, client.c to use ivshmem
```

**Effort**: ~2-4 hours to add ivshmem support

### Step 3: Optional Optimization
```
Eventually:
  - Replace eventfd with ivshmem doorbell
  - Use ivshmem server mode for peer notifications
  - (But the current approach with ivshmem + eventfd is already good)
```

---

## Quick Implementation Outline

### ivshmem_interface.h
```c
typedef struct {
    void *addr;              // Virtual address of shared memory
    size_t size;
    int pci_device;          // PCI handle to ivshmem device
    int doorbell_fd;         // File descriptor to doorbell
    // ... other fields
} IVShmem;

IVShmem *ivshmem_create(const char *name, size_t size, int is_creator);
IVShmem *ivshmem_map(const char *name, size_t size);
int ivshmem_notify(IVShmem *iv, int peer_id);
void ivshmem_cleanup(IVShmem *iv);
```

### ivshmem_interface.c
```c
IVShmem *ivshmem_map(const char *name, size_t size) {
    // 1. Find ivshmem PCI device
    // 2. Map BAR2 (the shared memory region)
    // 3. Return mapped address and metadata
    // 4. Setup doorbell if needed
}

int ivshmem_notify(IVShmem *iv, int peer_id) {
    // Write to doorbell register to trigger peer interrupt
}
```

### Modified shared_mem_buffer.c
```c
// Old:
SharedMemBuffer *shmem_create(void) {
    void *ptr = mmap(...);
}

// New:
SharedMemBuffer *shmem_create_ivshmem(IVShmem *iv) {
    // Just return the ivshmem-mapped region
    return (SharedMemBuffer *)iv->addr;
}
```

---

## Testing with ivshmem

### Quick Test Without SEV First

```bash
# Create test host directory
mkdir -p /tmp/test-ivshmem

# Data Holder VM
qemu-system-x86_64 \
    -object memory-backend-file,id=ivshmem_mem,\
            size=64M,share=on,mem-path=/tmp/test-ivshmem/data-holder \
    -device ivshmem-plain,id=ivshmem0,memdev=ivshmem_mem \
    ... [other options] ...

# Orchestrator VM (different terminal, or separate QEMU instance)
qemu-system-x86_64 \
    -object memory-backend-file,id=ivshmem_mem,\
            size=64M,share=on,mem-path=/tmp/test-ivshmem/data-holder \
    -device ivshmem-plain,id=ivshmem0,memdev=ivshmem_mem \
    ... [other options] ...
```

Both VMs now share 64MB of memory!

### Validate in Guest

```bash
# Inside guest
lspci | grep ivshmem
# Should show ivshmem PCI device

# Check /sys
ls /sys/devices/pci*/*/resource*
# BAR resources should be visible
```

---

## Decision: ivshmem + Ring Buffer

I recommend: **Use ivshmem for memory management + Keep ring buffer + Keep eventfd for sync**

**Why**:
1. ✅ QEMU handles all the complexity
2. ✅ SEV-transparent (no special code needed)
3. ✅ Simple to test (just launch script change)
4. ✅ Our ring buffer code stays almost unchanged
5. ✅ Can incrementally optimize (add doorbell later if needed)

**Effort**: Small wrapper (~200 lines) + update init calls

**Benefit**: Professional, production-ready solution

---

## Next Steps

1. **Decide**: Use ivshmem? (Recommended: YES)
2. **Design**: Finalize ivshmem_interface API
3. **Implement**: Create ivshmem_interface.h/c (~500 lines)
4. **Integrate**: Update server.c, client.c, startup code (~100 lines)
5. **Test**: Verify on regular KVM, then SEV

Want me to create the ivshmem_interface.h/c files?
