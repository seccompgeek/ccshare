# TGPU Study & SEV GPU Manager Architecture - Summary

**Date**: 2026-06-21  
**Status**: Study Complete → Ready for Phase 1 Implementation  
**Author**: Based on comprehensive codebase analysis

---

## Part 1: TGPU Architecture (What We Learned)

### Core Concept
TGPU enables **GPU sharing between multiple VMs** through:
- **Kernel MUX/DEMUX**: 20 patches enabling time-multiplexed GPU access
- **Context Switching**: VFIO containers per VM, IOMMU page table switching
- **Encrypted Communication**: SSL/TLS channels + shared memory optimization
- **Smart Data Transfer**: Physical address awareness for direct writes

### Key Innovation: GPU Travelling Between VMs
```
┌─────────────────┐
│   VM1           │
│  Has GPU        │ ← Run GPU workload
│  for 10ms       │
└────────┬────────┘
         │ Context switch (VFIO + IOMMU + Interrupt MUX)
         ↓ 1-10ms overhead
┌────────┴────────┐
│   VM2           │
│  Has GPU        │ ← Run GPU workload
│  for 10ms       │
└─────────────────┘
```

### Communication Speed Progression
- **Original (TCP)**: 1-10 ms roundtrip (network overhead)
- **New (Shared Memory)**: 10-100 μs (100x improvement)
- **Secret**: Non-private memory + eventfd + custom OpenSSL BIO

### TGPU Component Layout
```
Kernel Patches (tgpu-kernel-patch/)
├─ Multiple VFIO container support (0001, 0003)
├─ DMA multiplexing (0006, 0008)
├─ Interrupt MUX (0004, 0009, 0020)
└─ Context switching fixes (0010-0018)

Application (tgpu-llm.c/)
├─ Orchestrator VM: Runs GPU workload, requests data
├─ Data Holder: Feeds dataset via SSL
└─ Communication: SSL over TCP (old) → SSL over shared memory (new)

Existing in sev-changes/
├─ shared_mem_buffer.h/c: Ring buffer infrastructure
├─ shared_mem_bio.h/c: Custom OpenSSL BIO layer
└─ INTEGRATION_GUIDE.md: How to integrate into existing code
```

---

## Part 2: Your SEV GPU Manager Requirements Analysis

### What You're Building
A **management layer** on top of tgpu for SEV systems:

```
Manager CVM (Holds GPU)
    ↓ schedules access
    ├─→ CVM #1 (Request → Compute → Release)
    ├─→ CVM #2 (Request → Compute → Release)
    ├─→ VM #3  (Request → Compute → Release)
    └─→ VM #4  (Request → Compute → Release)
```

### Key Challenges
1. **SEV Isolation**: CVMs can't directly see each other's memory
   - Solution: Use non-private shared regions (manager allocates)
   
2. **No Host Detours**: Minimize hypervisor involvement
   - Solution: Manager-driven scheduling, batch context switches
   
3. **Performance**: Sub-millisecond request latency
   - Solution: Hybrid communication (control + data planes)
   
4. **Scalability**: Support 4-8 concurrent VMs fairly
   - Solution: Priority-aware scheduler, fairness enforcement

### Design Principle: Avoid Naive Approaches
```
❌ BAD: Each VM does ivshmem doorbell to manager
        └─ Scales poorly, no backpressure, hard to prioritize

❌ BAD: Everything via host hypervisor
        └─ Too many CPUID exits, hypervisor becomes bottleneck

✅ GOOD: Manager controls shared memory allocation
         └─ VMs access pre-shared regions, SSL-encrypted data
         └─ eventfd-based synchronization (proven in tgpu)
         └─ Explicit scheduling policy (fairness, priority)
```

---

## Part 3: Recommended Architecture (HYBRID APPROACH)

### Three Communication Planes

#### 1. Control Plane (ivshmem doorbells)
**Purpose**: Fast notifications only  
**Latency**: 1-5 μs  
**Payload**: 32 bits (simple notifications)  
**Use Cases**:
- Liveness heartbeat ("I'm still here")
- Urgent request flag ("GPU needed now!")
- Status queries ("Is GPU available?")

```c
// Control message (fits in 32 bits)
struct {
    uint8_t vm_id;      // 8 bits
    uint8_t urgent;     // 1 bit
    uint16_t status;    // 15 bits
} = 32 bits total
```

**Via**: QEMU ivshmem device (standard, mature, works on SEV)

---

#### 2. Request Plane (custom shared memory + eventfd)
**Purpose**: GPU requests, grants, scheduling info  
**Latency**: 10-100 μs  
**Payload**: 256+ bytes (full request structure)  
**Use Cases**:
- VM submits GPU request with duration, priority
- Manager grants GPU time slice
- Status updates (queue position, ETA)

```c
// Request message (256 bytes)
struct {
    uint8_t vm_id;
    uint8_t priority;
    uint16_t msg_type;
    uint32_t duration_us;
    uint32_t required_vram_mb;
    uint64_t timestamp_ns;
    uint8_t data[100];  // Opaque GPU command
} = 256 bytes (fits in ring buffer)
```

**Via**: Ring buffer in non-private memory + eventfd signaling (proven in tgpu)

---

#### 3. Data Plane (direct shared memory)
**Purpose**: Bulk data transfer (models, gradients)  
**Latency**: 1 μs/MB (direct memory access)  
**Payload**: MB+ (streaming data)  
**Use Cases**:
- Orchestrator uploads GPU kernel code
- Data holder transfers batch to GPU memory
- Results downloaded to VM

```
┌──────────────────────────────┐
│ Manager CVM (allocates)      │
│  ├─ VM1 buffer (256 MB)      │
│  ├─ VM2 buffer (256 MB)      │
│  └─ VM3 buffer (256 MB)      │
└──────┬───────────────────────┘
       │ exports physical addresses
       ↓
VMs map and use directly (via SSL if sensitive)
```

**Via**: Pre-allocated shared buffers, optional SSL encryption

---

### Why Hybrid?

| Component | ivshmem Only | Custom Only | Hybrid |
|-----------|-------------|-----------|--------|
| Control latency | 1-5 μs ✅ | 10-100 μs | 1-5 μs ✅ |
| Dev complexity | Low ✅ | High | Medium |
| Data scalability | Poor ❌ | Good ✅ | Good ✅ |
| Ring buffer needed? | No | Yes | Yes (for request/data) |
| Encryption support | No ❌ | Yes ✅ | Yes ✅ |
| **Verdict** | Fast but limited | Complex but proven | **BEST BALANCE** |

---

## Part 4: Implementation Roadmap

### Phase 1: Control Plane (2 weeks)
**Goal**: Manager daemon receives GPU requests, awards GPU via scheduling

**Deliverables**:
- Manager daemon process
- Request queue (FIFO)
- Round-robin scheduler
- ivshmem integration
- Basic tests

**Estimated LOC**: 1500-2000 lines

**Success**: 3+ VMs submitting requests, receiving grants, <500 μs latency

---

### Phase 2: Data Plane (2 weeks)
**Goal**: VMs exchange data via shared memory + SSL

**Deliverables**:
- Shared memory allocator (manager-controlled)
- Per-VM data buffers
- SSL-over-shared-mem (reuse tgpu code)
- Routing table for VM pairs

**Estimated LOC**: 2000-2500 lines (mostly from tgpu)

**Success**: 4+ VMs transferring MB/sec without corruption, >1 Gbps throughput

---

### Phase 3: Integration (1 week)
**Goal**: Full multi-VM scheduling + data routing

**Deliverables**:
- Priority scheduling (not just round-robin)
- Fairness enforcement
- Liveness detection
- Dynamic VM registration

**Estimated LOC**: 1000-1500 lines

**Success**: 8 VMs competing, GPU utilization >80%, fair distribution

---

### Phase 4: Optimization (ongoing)
**Goal**: Sub-millisecond latency, production-ready

**Deliverables**:
- Lock-free data structures
- NUMA-aware allocation
- Batch scheduling
- Performance profiling

**Total Timeline**: 5-6 weeks to MVP

---

## Part 5: Key Architectural Decisions

### Decision 1: Manager-Centric (Not Peer-to-Peer)
✅ **Chosen**: Central manager CVM controls GPU access  
❌ Alternative: Each VM negotiates directly (too complex, unfair)

### Decision 2: Shared Memory, Not TCP
✅ **Chosen**: Non-private regions (10-100x lower latency than TCP)  
❌ Alternative: Network-based (Virtio-Net adds 1-10 ms)

### Decision 3: eventfd Synchronization
✅ **Chosen**: Blocked waits with event notifications  
❌ Alternative: Polling (wastes CPU, higher latency)

### Decision 4: SSL Encryption Over Non-Private Memory
✅ **Chosen**: Application-level encryption (SEV doesn't encrypt non-private)  
❌ Alternative: Rely on SEV alone (not available for non-private)

---

## Part 6: Files to Create

### Phase 1 Directory Structure
```
/sev-changes/
├── include/
│   └── sev_gpu_manager.h           (~200 LOC, data structures)
│
├── manager/
│   ├── manager.c                   (~500 LOC, main daemon)
│   ├── scheduler.c                 (~300 LOC, scheduling policy)
│   ├── manager.h                   (~100 LOC, types)
│   └── Makefile
│
├── vm-client/
│   ├── request.c                   (~200 LOC, VM-side API)
│   ├── request.h                   (~50 LOC)
│   └── Makefile
│
├── tests/
│   ├── test_single_request.c       (~150 LOC)
│   ├── test_multi_vm.c             (~300 LOC)
│   └── Makefile
│
├── Makefile                         (top-level)
├── README.md                        (getting started)
├── DESIGN.md                        (architecture)
└── STUDY_SUMMARY.md                (this file)
```

### Phase 2 Additions
```
data-plane/
├── shared_mem_allocator.c          (~300 LOC)
├── routing_table.c                 (~200 LOC)
├── Makefile
│
├── (from tgpu-llm.c/ssl/)
│   ├── shared_mem_buffer.c/h       (copy as-is)
│   ├── shared_mem_bio.c/h          (copy as-is)
│   └── server.c                    (adapt for multi-VM)
```

---

## Part 7: Performance Targets

| Metric | Target | Path |
|--------|--------|------|
| **Control latency** | <100 μs | ivshmem doorbells |
| **Request latency** | <500 μs | Shared memory + eventfd |
| **Grant latency** | <1 ms | Manager scheduling |
| **Data throughput** | >1 Gbps | Direct shared memory |
| **GPU utilization** | >80% | Reduce context switch overhead |
| **Fairness ratio** | <2x | Round-robin + aging |

---

## Part 8: Risk Mitigation

| Risk | Severity | Mitigation |
|------|----------|-----------|
| eventfd behavior on SEV | MEDIUM | Test early (Week 1), fallback to polling |
| Context switch overhead | MEDIUM | Profile real workload, optimize hot paths |
| Manager becomes bottleneck | MEDIUM | Batch scheduling, async processing |
| Memory fragmentation | LOW | Pre-allocate fixed-size regions |
| Scheduling unfairness | LOW | Implement aging/priority boosting |
| Manager crash | LOW | Heartbeat + VM re-registration |

---

## Part 9: Comparison Matrix

### TGPU vs Your System

| Aspect | TGPU | Your SEV Manager |
|--------|------|-----------------|
| **GPU Owner** | Ephemeral (VM decides) | Persistent (manager decides) |
| **Clients** | 2 (orchestrator + data holder) | N (4-8 typical) |
| **Scheduling** | Implicit (orchestrator) | Explicit (manager policy) |
| **Communication** | SSL over TCP/shared-mem | Hybrid (ivshmem + shared-mem + SSL) |
| **Complexity** | Medium (proven design) | High (new scheduler) |
| **Fairness** | Depends on orchestrator | Built-in (manager enforces) |
| **Scalability** | O(2) simple | O(N) with careful design |

**Why Inherit from TGPU**: Ring buffer, shared memory BIO, SSL integration all proven in tgpu code. Copy, adapt, extend.

---

## Part 10: Next Steps

### This Week
1. Create directory structure
   ```bash
   mkdir -p /sev-changes/{include,manager,vm-client,tests}
   ```

2. Copy Phase 1 data structures from session memory (sev_gpu_manager.h)

3. Review tgpu kernel patches for VFIO context switching insights

### Next Week
1. Implement manager daemon skeleton
2. Basic request queue and round-robin scheduler
3. Unit tests for control plane

### Following Week
1. Integrate shared memory allocator
2. Begin Phase 2 data plane work
3. Stress test with multiple VMs

### Success Metrics (Week 3-4)
- [ ] 3+ VMs requesting GPU
- [ ] Manager awards GPU in fair order
- [ ] Request latency < 500 μs
- [ ] No deadlock or memory leaks
- [ ] Repeatable across 100+ iterations

---

## Summary: What Makes This Design Good

1. **Proven Foundation**: TGPU already does GPU MUX; we're just adding management
2. **Hybrid Communication**: Optimized for each message type
3. **Fair Scheduling**: Explicit manager prevents starvation
4. **Low Latency**: ivshmem (1-5 μs) + shared memory (10-100 μs)
5. **Scalable**: Can support 8+ VMs without major redesign
6. **Secure**: SSL over non-private memory, manager controls allocation
7. **SEV-Native**: Works with non-private memory model

---

## References

- [tgpu-llm.c/README.md](../../tgpu-llm.c/README.md) - tgpu overview
- [tgpu-llm.c/ssl/shared_mem_buffer.h](../../tgpu-llm.c/ssl/shared_mem_buffer.h) - Ring buffer design
- [SHARED_MEMORY_SSL_ARCHITECTURE.md](SHARED_MEMORY_SSL_ARCHITECTURE.md) - Communication architecture
- [INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md) - How to integrate into existing code

---

**Next**: Review session memory files and begin Phase 1 implementation!

