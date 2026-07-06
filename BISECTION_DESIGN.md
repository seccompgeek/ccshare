# SEV-GPU Remoting — Formal Bisection Architecture

Status: DRAFT for review. This is a design specification, not an implementation.
It defines *where* each piece of GPU driver state must live (client vs manager)
and *why*, so that code changes follow the structure instead of the structure
emerging from ad-hoc short-circuits.

---

## 0. The single invariant

> **Anything that executes on, or holds live state inside, the physical GPU is
> manager-side. The client may only decide, reserve, bookkeep, and forward.**

Every current failure traces to a short-circuit that violated this by choosing
one of two wrong answers instead of bisecting:
- "pretend it succeeded locally" (used for `register_gpu`, `register_gpu_va_space`,
  `register_channel`, `RetainChannel`) — breaks because real GPU state was needed;
- "forward blindly" — would block the client on absent hardware.

The correct third answer is **bisect**: split each operation into a client-side
*control* half (handles, VA reservation, shadow pages, polling) and a manager-side
*execution* half (channels, page tables, keys, scheduling, work launch), joined by
an explicit RPC.

---

## 1. The two UVM planes (verified in code)

### 1a. Internal UVM — the control/management plane
`uvm_channel_manager_t`, created by `uvm_channel_manager_create()`
([uvm_gpu.c:1661]) during `uvm_gpu_retain_by_uuid → uvm_gpu_init`. Pools
([uvm_channel.h:123-146]):

| Pool type | Purpose |
|---|---|
| `CE` / `CE_PROXY` | migrations, scrub, memset, `MEMOPS` (TLB invalidate), P2P |
| `SEC2` | CC root-of-trust; bootstraps WLC/LCIC with signed pushes |
| `WLC` | CC "decrypt-then-run" work launch |
| `LCIC` | CC launch-confirmation / completion semaphores |

These are **the driver's own channels**. They run on the GPU. They are the engine
that actually *performs* a confidential submission (WLC decrypt-then-run) and the
memory movement across the CPR boundary (CE).

### 1b. Normal/User UVM — the workload plane
`uvm_user_channel_t` registered via `UVM_REGISTER_CHANNEL`, tracked per
`gpu_va_space` for fault attribution; plus the managed-memory VA spaces. This is
what CUDA drives for actual compute.

### 1c. Consequence — internal UVM is a *skeleton* on the client, not absent

The naive reading is "no GPU ⇒ no internal UVM on the client." That is wrong, and
it is precisely the mistake the current short-circuits make: they return `NV_OK`
and create *nothing*, leaving the Normal UVM plane with no object graph to bind to
(no `uvm_gpu_t` ⇒ `get_gpu_by_uuid` fails ⇒ `register_channel` can't resolve ⇒ it
too must be stubbed — the hollow cascade we observe).

The correct statement is a split of internal UVM by **content vs structure**:

- **Structure (object graph) is required on BOTH sides.** Normal UVM is defined in
  terms of internal-UVM objects — it registers a user channel *against* a
  `uvm_gpu_t`, looks up a `gpu_va_space`, references `gpu->channel_manager` and the
  page tree. Those objects must EXIST on the client or the workload plane cannot be
  expressed at all.
- **Content (GPU-resident, secret-bearing state) lives ONLY on the manager.** Real
  channels, KMBs/keys, CPR/vidmem addresses, page-directory physical addresses,
  work-submit tokens for *other* clients — none of these may be on the client.

So the client's internal UVM is a **secret-free skeleton**: the full object graph,
populated with placeholders and forwarding stubs, whose GPU-touching operations
RPC to the manager. "Placeholder is okay" — the client needs the *shape*, not the
*potency*.

- **CC work launch is still a manager operation** (SEC2/WLC/LCIC are content, not
  structure). The client's skeleton WLC/LCIC pools may exist as placeholders, but
  the actual decrypt-then-run executes on the manager. A client cannot launch CC
  work locally — not because it lacks the objects, but because it lacks the keys
  and the GPU.

---

## 2. The four-quadrant model (plane × side)

|                | Client side (no GPU)                                   | Manager side (owns GPU, N clients)                              |
|----------------|--------------------------------------------------------|-----------------------------------------------------------------|
| **Internal UVM** | **secret-free skeleton**: `uvm_gpu_t` proxy, `channel_manager` shell, CE/SEC2/WLC/LCIC channels that are either no-work stubs *or* bound to shared slots (§2.2) if exercised, placeholder page tree; GPU-touching ops forward | real channel_manager: CE/SEC2/WLC/LCIC; performs all CC launches & CE copies on behalf of clients using each client's KMB |
| **Normal UVM**  | VA reservations, handle map, shadow USERD/doorbell pages, completion polling, fault *forwarding* — all bound to the client's skeleton internal-UVM objects | real per-client GPU VA spaces + page tables; user-channel alloc/bind/schedule; work-submit token generation |

The client's internal-UVM quadrant is a **skeleton, not empty**: it holds the
object graph (structure) so the Normal UVM plane has something to bind to, but no
content (keys, channels, GPU addresses). That is the formal statement of *"client
control is not very potent but good enough to drive things."*

### 2.1 The client internal-UVM skeleton — contents vs exclusions

| Object | Client skeleton HOLDS (structure, non-secret) | Client skeleton MUST NOT HOLD (content → manager) |
|---|---|---|
| `uvm_gpu_t` proxy | UUID, arch/caps, engine masks, `mem_info` (size as reported by manager), flags | real BAR mappings, register apertures |
| `channel_manager` | pool array shape, channel counts, type→pool map | real channels bound to the GPU |
| CE/SEC2/WLC/LCIC pools | channel objects **bound to shared-memory slots** (see §2.2), not hollow; unused-on-client ones may be no-work stubs | KMBs / CC keys, real engine binding + runlist schedule |
| page tree (`address_space_tree`) | placeholder root so VA-space objects link | real page-directory physical addresses / PTEs (on manager) |
| `gpu_va_space` | handle + reserved VA ranges | the real VA space object (manager) |
| `user_channel` | handle, VA range, **USERD/GPFIFO/pushbuf pointers into its ivshmem slot**, instance info from manager RPC (shadow doorbell, mgr token) | real retained channel, resource bindings, engine schedule |

Rule of thumb: the client may hold anything a compromised client could already
compute or that is scoped to *itself* (its own handles, its own VA layout, its own
work-submit token that the manager validates by ownership). It may never hold a
key, another client's state, or a physical address inside the GPU/CPR.

### 2.2 A channel is three-part — the shared-memory data plane is the bridge

A "channel" is not client-vs-manager; it spans **three** places:

1. **Control object** — client proxy (handle, bookkeeping, pointers into the shared
   slot) ⟷ manager real channel (the RM `KernelChannel`).
2. **Data plane** — lives in **shared ivshmem**, mapped by *both* peers: USERD
   (GP_PUT/GP_GET), GPFIFO ring, pushbuffer, CE bounce buffers, shadow doorbell,
   completion semaphore. This is the substrate the client writes and the GPU reads.
3. **Engine binding + runlist schedule** — manager only (real GPU).

The link to the GPU is #2: the manager constructs the real channel with its
USERD/GPFIFO/pushbuffer **pointing at the same shared physical pages** the client's
proxy maps (via the osdesc/shared-GPA carves). So a client channel proxy is
**bound**, not placeholding — the CPU-side of a real, GPU-attached channel whose
control lives on the manager and whose data lives in shared memory.

"No-work placeholder" is therefore a *narrow* case: it applies **only** to an
internal-UVM channel that the client never pushes work on (pending P0's answer to
"does any internal push run on the client?"). Any channel that carries work — every
user/workload channel, and any internal channel that turns out to be exercised on
the client — must be a **bound** channel with a shared-memory data plane and a
manager real channel behind it. If P0 shows an internal push does run on the
client, that channel cannot be a stub; it must forward to a manager real channel
over its shared slot.

Per-VM ivshmem region layout (manager constants, low→high):

```
[data header @0]
[free / CE bounce buffers            ~low 29 MiB]     <- CC secure-copy staging
[osdesc reserve                       32 MiB]         <- CUDA CPU buffers (esc 0x27)
[compute reserve:
   MAX_CHANNELS_PER_VM(32) x CHAN_STRIDE(3 pages)]    <- per-channel USERD|GPFIFO|pushbuf
   + shadow doorbell window (64 KiB)]                 <- usermode VF page (+0x80 timer, +0x90 db)
[RPC staging window                   @top]
```

Both peers derive this identical geometry from the shared constants
(`SEV_GPU_COMPUTE_CHAN_STRIDE`, `SEV_GPU_COMPUTE_RESERVE_SIZE`,
`SEV_GPU_OSDESC_RESERVE_SIZE`), so a channel's slot index maps to the same physical
pages on both sides — which is exactly how the proxy binds to the GPU. **Any
channel model in this design must allocate from these pools; a channel with no
shared-memory slot cannot reach the GPU.**

---

### 2.3 Submission mechanics — the doorbell register, `workSubmissionOffset`, the token

**One doorbell register per GPU.** `NV_VIRTUAL_FUNCTION_DOORBELL` at **BAR0 + 0x30090**
(Blackwell, `dev_vm.h`) — equivalently offset **0x90** (`NVC361_NOTIFY_CHANNEL_PENDING`)
within the usermode/VF region, which is itself BAR0 + 0x30000. **Every channel rings
the same register**; the written value (token) selects which channel. So the manager
needs only the one VF doorbell; it does not need per-channel doorbell registers.

**`workSubmissionOffset` is a POINTER to that register, not an offset value** (the
name misleads). RM sets it two equivalent ways:
- `bar0Mapping + 0x30090` ([nv_gpu_ops.c:10395])
- `clientRegionMapping + 0x90` ([nv_gpu_ops.c:5631])

i.e. *(mapping base) + (doorbell's fixed offset)*. "Offset from what" = from the BAR0
base (0x30090) or, identically, from the usermode-region base (0x90).
`workSubmissionOffsetGpuVa = clientRegionGpuAddr + 0x90` ([nv_gpu_ops.c:6239]) is the
same register aliased in **GPU VA**, used for the WLC's GPU-side self-rings.
- Client (remoting): `shadow_page + 0x90`.
- Manager: real `BAR0 + 0x30090`.

**The token is the value written; it NAMES the channel.** On Blackwell the doorbell
value is a bitfield (`dev_vm.h` gb202): `RUNLIST_DOORBELL`(30) | `RUNLIST_ID`(22:16) |
`VECTOR`(11:0). `kfifoGenerateWorkSubmitToken` builds it from the channel's
`(runlist_id, chid)` — so a token is specific to **the channel AND the GPU/RM it
lives on**. A client-generated token is meaningless on the manager's GPU.

**How the token is used (the secure rule):**
1. **Generation = manager only.** `GET_WORK_SUBMIT_TOKEN` (esc 0x2a, cmd 0xc36f0108)
   is forwarded → manager runs `kfifoGenerateWorkSubmitToken` on its real channel →
   returns a manager-valid token to the client.
2. **Ring = manager only.** On a submission signal, the manager **regenerates** the
   token for the channel it has validated the client owns (`assign_state`), then
   `GPU_VREG_WR32(NV_VIRTUAL_FUNCTION_DOORBELL, token)`.
3. **The client's shadow-doorbell write is a SIGNAL, not a trusted token.** The
   manager never rings with a client-supplied value — otherwise a malicious client
   could write another client's `(runlist_id, chid)` token and ring a channel it does
   not own. *Regenerate-from-owned-channel* is the isolation guarantee, and it is the
   concrete reason the manager keeps submission control (§6 Q1): it is sole authority
   over both the token (channel identity) and the one physical doorbell.

Same rule for WLC: the manager rings the WLC's doorbell with the WLC channel's own
regenerated token; the client only stages the WLC push in its sysmem slot.

## 3. Component disposition (the bisection table)

| Component | Client half | Manager half | RPC? |
|---|---|---|---|
| GPU registration (`register_gpu`) | build `uvm_gpu_t` proxy + `channel_manager` skeleton (placeholder pools/channels) so Normal UVM can bind | create real per-client GPU context + channel_manager (internal UVM) | **yes** |
| GPU VA space (`register_gpu_va_space`) | keep `hVaSpace`, reserve VA ranges | create real VA space + page directory | **yes** |
| User channel (`register_channel`) | keep handle, own shadow USERD/GPFIFO carve + doorbell page | alloc + bind + schedule real channel; seal KMB; return {shadow_db, mgr_token, completion_sem} | **yes** |
| Work-submit token | write mgr_token to shadow doorbell | regenerate token for *its* CHID/runlist (`kfifoGenerateWorkSubmitToken`) | via channel-create result |
| Work submission | advance GP_PUT in shared USERD; signal | run secure launch (WLC decrypt-then-run / CE copy) with client KMB, ring real doorbell | **yes** (trigger TBD, §6) |
| KMB / CC keys | receive sealed KMB, encrypt pushbuffer/data | seal per (vm, channel, keyspace, seq) — already built (D4.2b) | existing |
| External alloc / DMA map | reserve GPU VA | program PTEs on real GPU (`UVM_MAP_DMA`) | existing |
| Completion semaphore/notifier | poll shared-ivshmem page | place channel sem/notifier in the client's ivshmem carve | design |
| Timer (PTIMER 0x80/0x84) | read from shadow page | emulate advancing clock into shadow page (hrtimer) | existing |
| Managed-memory faults | forward fault to manager | service on real GPU | **deferred** |

---

## 4. The RPC boundary (the only things that cross)

Minimal, `client_id`-scoped, idempotent:

1. `RegisterGpu(client_id) → {proxy_desc, shadow_geometry}`
2. `RegisterVaSpace(client_id, hVaSpace) → {handle_map}`
3. `CreateWorkChannel(client_id, engine, hVaSpace, carve) → {shadow_db_gpa, mgr_token, completion_sem_gpa}`
4. `SubmitWork(client_id, channel, gp_put) → status`  *(trigger mechanism = open question §6)*
5. `MapExternal / MapDma(client_id, …)`
6. `FreeChannel / Teardown(client_id, …)`

Everything else stays client-local (VA math, handle tables, shadow-page mmap,
polling). No client operation may block on GPU hardware; each GPU-needing step is
exactly one of the above.

---

## 5. Multi-client structure

Existing manager machinery to build on:
- `assign_state.a[MAX_VMS][MAX_CHANNELS_PER_VM]` — per-client channel ownership registry.
- D4.2b provisioner pool — pre-allocated CC channels per client.
- KMB sealing AAD-bound to `(vm_id, channel, keyspace, seq)`.
- Per-VM ivshmem carves (`data_devs[]`, `client_mem_phys_cache[]`, osdesc cursors).

Isolation requirements per RPC:
- **Ownership**: validate `(client_id, channel)` against `assign_state`; a client
  may only submit to channels the manager assigned it (`enforce_channel_ownership`).
- **VA space**: one GPU VA space per client; never share PTEs.
- **Keys**: per-client KMB (already AAD-bound).
- **Tokens**: manager regenerates for its own channels ⇒ a client cannot ring
  another client's channel even if it forges a token.
- **Fairness (new)**: per-client channel quota (`MAX_CHANNELS_PER_VM=32`) and a
  submission scheduler so one client cannot starve the GPU.

Granularity decision (leaning per-client, pending feasibility): one real
`uvm_gpu_t` / RM-client context per client gives strong isolation at higher GPU
resource cost. The alternative — one shared context partitioned by VA space +
channel pool — is lighter but leans entirely on ownership checks for isolation.

---

## 6. Open questions to resolve BEFORE coding

These are the "needs checking" items; each blocks a design choice.

**Q1 — Is CUDA's usermode mapping actually the shadow page? (timer vs doorbell)**
The manager writes a fake PTIMER to `shadow+0x80/0x84` and reads the same page in
its scan — that only proves the manager sees its *own* writes. It does **not**
prove CUDA reads/writes that page. Concrete check: confirm
`"sev_gpu: mmap_redirect doorbell pfn="` fires in the *client* dmesg (the client's
`mmap` of the usermode object resolved to the shadow pfn). If it fires and
cudaMalloc still advances past the timer gate, CUDA *is* on the shadow page — and
then the absence of any `+0x90` write proves CUDA does not ring the usermode
doorbell for CC work. If it does *not* fire, the mapping itself is broken and the
"trap" never happened — consistent with the hypothesis that the missing
`register_gpu` stage left the usermode mapping half-wired.

**Q2 — Where does CUDA's CC compute submission actually go?**
libcuda is closed-source and its submit path does not enter the client kernel, so
neither an ioctl hook nor a kernel trace can observe it. We must determine
empirically (native run instrumentation) whether the compute channel is launched
by (a) a userspace `+0x90` doorbell write, or (b) an on-GPU WLC self-ring. This
decides whether `SubmitWork` can be triggered by trapping a client write at all,
or must be driven by the manager polling GP_PUT / an injected shim. **We cannot
rely on an explicit per-submission RPC alone**, because CUDA does not call the
kernel to submit.

**Q3 — WLC/LCIC granularity. [RESOLVED — per-client, code-verified]**
Correction of an earlier wrong claim: WLC/LCIC **can and should be per client.**
The `min(num_used_ces × 2, 16)` figure is only how *UVM* sizes its *own* internal
pool for its *own* work — not a GPU limit. The manager provisions per-client
channels through RM directly (`rm_sev_gpu_alloc_compute_channel`,
`rm_sev_gpu_alloc_cc_channel`, [osapi.c:1966,2225]); extending that to a per-client
**WLC/LCIC pair set** gives per-client keyed launch with no shared bottleneck.

- **Budget, not blocker.** Hardware exposes the ceiling:
  `NV_CONF_COMPUTE_CTRL_CMD_GPU_GET_NUM_SECURE_CHANNELS` ([ctrlcb33.h:365-391])
  → `maxSec2Channels`, `maxCeChannels`. Per-client WLC/LCIC/CE draw from
  `maxCeChannels`; `N_clients` is capped by that number. **Query it (P0).**
- **SEC2 is shared — non-negotiable.** Only `maxSec2Channels` (small; UVM makes 1)
  SEC2 exist. SEC2 is the bootstrap anchor only (not in the launch path); one
  shared SEC2 bootstraps every client's per-client WLC/LCIC sequentially.
- **Compute/user channel ring does not use WLC** — manager rings it directly
  (`kfifoRingChannelDoorBell` + regenerated token). WLC/LCIC are for the keyed
  **H2D/D2H data copies** (decrypt/encrypt across the CPR boundary).

### Q3a — Can the client write WLC / read LCIC over shared ivshmem? [YES — verified]
CC already splits WLC/LCIC memory exactly along our boundary:

| WLC/LCIC memory | Location | Side |
|---|---|---|
| WLC per-launch pushbuffer + auth tags (`WLC_SYSMEM_TOTAL_SIZE × num_channels`) | **unprotected sysmem**, CPU-mapped ([uvm_channel.c:2853-2856]) | **client writes** → shared ivshmem slot |
| WLC static schedule (the fixed decrypt-then-run program) | **protected vidmem/CPR** ([uvm_channel.c:841,2862]) | manager only (set up once by SEC2) |
| LCIC tracking semaphore / notifier | **sysmem** (`pool_sysmem`, [uvm_channel.c:1090,1106]) | **client reads** → shared ivshmem |

So writing WLC and reading LCIC over shared memory is the **same pattern as CE +
bounce buffers** — the client-facing WLC/LCIC structures are already unprotected
sysmem by CC design; only the secret static schedule stays in CPR. Grounding =
point those sysmem allocations at the client's ivshmem slots.

### Q3b — Is the WLC/LCIC doorbell special (mapping)? [NO — verified]
- The WLC doorbell is the **standard VF doorbell** (`workSubmissionOffset`, written
  by the CPU, [uvm_channel.c:1178]) — same mechanism as every channel. Not special
  in kind.
- WLC/LCIC are **primed** (`PUT == GET + 2`, [uvm_channel.c:1131]): once set up, a
  **single** WLC doorbell ring triggers the whole decrypt-then-run + target launch
  + LCIC completion. So there is exactly ONE doorbell to handle per launch.
- The **LCIC and target-channel rings are GPU-side** (semaphore release to
  `workSubmissionOffsetGpuVa`, executed by the WLC schedule on the GPU, e.g.
  [uvm_channel.c:1558]) — not CPU writes, not client-visible, no client mapping.
- Remoting disposition: identical to the compute channel — **the manager rings the
  WLC doorbell** (client has no BAR0); the client's "ring" reaches the manager via
  the submission-trigger (Q2). The GPU-side self-rings need the doorbell
  **GPU-VA-mapped on the manager** (`bInternalMmio`, [osapi.c:3324-3341]) — a
  manager-side setup, not a client concern.

**Q4 — Per-client `uvm_gpu_t` feasibility.**
Confirm the manager's UVM can hold N registered GPU contexts against one physical
GPU (or whether contexts must be virtualized on top of one channel_manager).

---

## 7. Phased plan

- **P0 — Boundary instrumentation + skeleton-surface map.**
  (a) Resolve Q1/Q2 empirically: log every client short-circuit hit and the
  `mmap_redirect`/timer/doorbell facts. *Gate: we know exactly what CUDA does at submit time.*
  (b) Produce the **skeleton-surface map**: the exact set of internal-UVM object
  fields and code paths that Normal UVM dereferences on the client, so §2.1 is
  backed by evidence and we know how thick the skeleton must be. A static first
  pass (grep the client-run paths for `gpu->` / `gpu_va_space->` / `channel_manager->`
  accesses) is confirmed/pruned by runtime instrumentation. See §8.
- **P1 — `RegisterGpu` bisection.** Replace [uvm_gpu.c:4029] with proxy + RPC;
  manager creates the real per-client GPU context + internal-UVM channel_manager.
- **P2 — VA space + user channel.** Replace [uvm_gpu.c:4067] and
  [uvm_user_channel.c:762] with forwards; delete the now-dead `RetainChannel` stub.
- **P3 — Work submission.** Wire `SubmitWork` per Q2's answer; manager runs the
  secure launch. *Gate: single-client `cc_compute_proof_client` prints PASS.*
- **P4 — Multi-client hardening.** Ownership on every RPC, per-client VA/scheduling,
  quotas, teardown/reset.
- **P5 — Deferred.** Managed-memory fault routing.

---

## 8. Skeleton-surface map (P0b — static first pass)

Decision locked: **hybrid reuse-and-gate** (supersedes the earlier "pure parallel
constructor" — reversed after reading the real constructor). Rationale: the entire
`uvm_gpu_t` is seeded from `gpu_info` (`nvUvmInterfaceGetGpuInfo`, [uvm_gpu.c:3053]),
and `add_gpu → init_parent_gpu → init_gpu` is *mostly HAL wiring and field-init we
want to keep*. Only ~5 sub-steps are GPU-only. So the real path runs on the client
with `gpu_info` forwarded from the manager, and these steps are gated behind the
single `nv_sev_is_client_mode()` predicate:
1. `nvUvmInterfaceRegisterGpu` — stub/forward (already stubbed on client)
2. `nvUvmInterfaceGetGpuInfo` — **forward: return manager-sourced `UvmGpuInfo`** (the seed)
3. ISR arm (`init_parent_gpu`) — skip
4. fault-buffer alloc/enable — skip
5. `channel_manager_create` (`init_gpu`) — skip → shell (or bound channels later)
6. ECC check — skip

A pure parallel constructor was rejected: it would duplicate ~200 lines of
HAL-wiring/field-init that drift from upstream. The boundary stays auditable as the
explicit gate list, not a fork of the object graph.

**Complete gate checklist** (corrected: ~14 sub-steps, not 5), all guarded by
`nv_sev_is_client_mode()`:

`init_parent_gpu` ([uvm_gpu.c:1449]):
- [ ] `nvUvmInterfaceDeviceCreate` → forward (manager creates rm_device)
- [ ] `nvUvmInterfaceGetFbInfo` → query, forward (already `bZeroFb`)
- [x] `uvm_hal_init_gpu` / `uvm_hal_init_properties` → **KEEP** (software HAL wiring)
- [ ] `uvm_pmm_devmem_init` → skip (HMM off)
- [ ] `uvm_pmm_gpu_device_p2p_init` → skip
- [ ] `uvm_parent_gpu_init_isr` → skip (no interrupts; completions come from manager)

`init_gpu` ([uvm_gpu.c:1574]):
- [ ] `nvUvmInterfaceDeviceCreate` (smc path) → skip (no SMC)
- [ ] `get_gpu_caps` → query, forward/default
- [ ] `alloc_and_init_address_space` → forward (manager owns the internal VA space)
- [ ] `get_gpu_fb_info` → query, forward
- [ ] `get_gpu_ecc_info` / `get_gpu_nvlink_info` → default
- [ ] `uvm_pmm_gpu_init` → shell (no vidmem)
- [ ] `init_semaphore_pools` → shell
- [ ] `uvm_channel_manager_create` → shell (or bound channels later)

**Shell vs NULL is runtime-driven.** Whether `pmm` / `semaphore_pools` /
`channel_manager` need functional shells or can be NULL depends on whether the
device-memory path (`register_gpu_va_space` + `register_channel`) dereferences them.
Build increment 1 with allocations skipped and shells NULL, instrument every gate,
then add a shell only where a guest run shows a deref. This is the concrete,
evidence-driven form of the P0b skeleton-surface map.

Method: grep the Normal-UVM paths that will run against the skeleton on the client
(`uvm_register_gpu_va_space`, `uvm_register_channel` → `uvm_user_channel_create`)
for `gpu->` / `gpu_va_space->` / `gpu->parent->` dereferences. This is a **static
candidate surface**; P0 runtime instrumentation confirms which are actually hit
for the device-memory proof and prunes the rest.

### Fields the skeleton must populate (grouped, with disposition)

| Object.field | Touched by | Skeleton disposition |
|---|---|---|
| `gpu->id` (84×) | va_space processor masks/bitmaps everywhere | **real index** — non-secret; must be a valid registered id |
| `gpu->uuid` | lookup, logging | real UUID (from manager) |
| `gpu->parent` | pervasive (`smc`, `id`, `isr`, numa, caps, `max_channel_va`, `peer_copy_mode`…) | placeholder `uvm_parent_gpu_t` with metadata; **`isr` needs care (no interrupts on client)** |
| `gpu->mem_info` | VA-space sizing, page-tree location | placeholder (size as manager reports; the reverted sysmem-page-table work lived here) |
| `gpu->conf_computing` | CC gating checks | **structure only — NO KEYS.** Provide enough to satisfy `enabled` checks; real keyspace on manager |
| `gpu->max_subcontexts` | channel subctx validation | scalar from manager caps |
| `gpu->instance_ptr_table_lock`, `gpu->instance_ptr_table` | channel instance-ptr insert | real lock + table (client-local bookkeeping; keys off the manager-returned instance ptr) |
| `gpu->test` | test hooks | zeroed |
| `gpu_va_space->gpu / ->va_space` | back-pointers | real (link skeleton objects) |
| `gpu_va_space->page_tables` | page-tree ops | **placeholder tree** — real PTEs on manager; map ops forward |
| `gpu_va_space->did_set_page_directory` | PDB set flag | placeholder true (manager owns the real PDB) |
| `gpu_va_space->ats` | ATS gating | off (device-mem proof) |
| `gpu_va_space->registered_channels / ->channel_va_ranges` | channel tracking | real client-local lists |
| `gpu_va_space->state / ->kref / ->deferred_free` | lifecycle | real client-local |

### Two sharp spots (flag for design, not code yet)

1. **`gpu->parent->isr`** — the interrupt service structure. The client has no GPU
   interrupts, so any Normal-UVM path that arms/waits on ISR against the skeleton
   must be redirected (faults/completions arrive via the manager, not a local IRQ).
   P0 must confirm whether the device-mem proof path touches `isr` at all.
2. **`gpu->conf_computing`** — this is exactly the secret-bearing boundary. The
   skeleton provides the *shape* (so `g_uvm_global.conf_computing_enabled` and
   per-gpu CC gates evaluate), but the CSL contexts / keys / IV state stay on the
   manager. Getting this line right is the crux of "structure not content."

### Init-dependency audit (pre-flip gate — verified against source, not runtime)

The skeleton skips most of `init_gpu`/`init_parent_gpu`. For each field the
downstream client path (`register_gpu_va_space`, `register_channel`, `add_gpu`
tail) dereferences, map it to its init site and bucket:
- **(A)** already initialized in a path the client runs
  (`alloc_gpu` / `alloc_parent_gpu` / `fill_parent_gpu_info` / kept-`init_parent_gpu`) → nothing to do
- **(B)** gap → add a shell in `sev_client_init_gpu`
- **(C)** gap → gate the use in the register path (P1b/P1c)

**Scope:** P1a-flip runs ONLY `uvm_va_space_register_gpu` (retain → `add_gpu` +
tail). `register_gpu_va_space` (P1b) and `register_channel` (P1c) stay stubbed. So
this audit covers `add_gpu` + `uvm_va_space_register_gpu` only.

| Field / step | Where / why | Verdict |
|---|---|---|
| `parent_gpu->instance_ptr_table_lock`/`table` | init in `alloc_parent_gpu` — a client-run path ([uvm_gpu.c:1234]) | **A** |
| `isr.replayable_faults.handling` (fault-enable, tail) | isr zeroed → false → block skipped | **A** |
| `uvm_gpu_check_ecc_error` (tail) | `!ecc.enabled` → NV_OK early ([uvm_gpu.c:2273]) | **A** |
| peer discovery (`peers_discover_static_link`, `register_gpu_peers`) | single GPU → no other parents → no-op | **A** |
| processor-mask bookkeeping | `gpu->id` + parent fields (fill_parent_gpu_info / init_parent_gpu-kept) | **A** |
| `mem_info.numa.enabled` | zeroed → numa-disabled path | **A** |
| access-counters enable ([uvm_va_space.c:758]) | needs ISR | **C — gated** |
| CC teardown DMA buffer alloc ([uvm_va_space.c:808]) | pool uninit on skeleton; manager owns teardown | **C — gated** |
| access-bits init ([uvm_gpu.c:3059]) | programs GPU state | **C — gated** |
| `max_subcontexts`, `time.time*_register` | used only by `register_channel`/timer (P1c, not P1a) | **soft — defer to P1c** |

**P1a-flip: DONE.** Short-circuit removed + 3 C-gates in place. Every `add_gpu` /
`uvm_va_space_register_gpu` deref is bucket A or gated. Ready for guest build+run.
(Correction from the first pass: the CC `dma_buffer_pool` gap is in
`uvm_va_space_register_gpu`, so it's a **P1a** gate, not P1b.)

### Open (needs P0 runtime confirmation)
- Does the device-memory proof path enter any internal-channel push on the client
  (which would require a *functional*, not placeholder, channel)? Expectation: no —
  device memory + forwarded maps shouldn't push internal work — but this must be
  proven, because it decides whether placeholder channels suffice or the skeleton
  must forward a live push.

## References (code)
- Short-circuits: `uvm_gpu.c:4029`, `uvm_gpu.c:4067`, `uvm_user_channel.c:762`,
  `nv_uvm_interface.c:1479/1538/1561/1577`, `nv-mmap.c:574`.
- Internal UVM: `uvm_channel.h:123-146`, `uvm_channel_manager_create` `uvm_gpu.c:1661`.
- Manager multi-client: `sev_gpu_manager.c` `assign_state`, D4.2b provisioner, KMB AAD.
- Doorbell shadow: `nv.c:4239` (MAP intercept), `sev_gpu_manager.c:5503`
  (`shadow_db_impl`), `sev_gpu_manager.c:6506` (`mmap_redirect_impl`),
  `sev_gpu_manager.c:6559` (timer).
