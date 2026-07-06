# SEV GPU Manager — Project Plan & Handoff

> Portable handoff for working on this from another workspace (e.g. one connected
> directly to the host/server). Self-contained: architecture, current status,
> deployment, and the verification ladder. Read this first.

---

## 1. What this project is

**Goal:** A SEV-based GPU sharing system. A **manager CVM** physically owns the
GPU; **client CVMs / VMs** request GPU access *through* the manager CVM. The
cross-VM communication is implemented as a **kernel module**.

**Important:** `tgpu/` (and `tgpu-llm.c`, `tgpu-kernel-patch`, `launch_vm.sh`,
etc. in the parent workspace) is **reference only**. Its TDX/VFIO setup is *not*
the target. Our work lives entirely under `sev-changes/`. Target platform is
**AMD SEV-SNP**.

---

## 2. Architecture (LOCKED)

Communication substrate = **QEMU ivshmem-doorbell** (PCI `1af4:1110`). This is the
only mechanism that maps the *same physical host RAM* into multiple SEV guests.

- **BAR0** = device registers (IntrMask/IntrStatus/IVPosition/Doorbell)
- **BAR1** = MSI-X table
- **BAR2** = shared memory (header + request region + grant region + per-VM data)

**Hybrid control/data split:**
- **Doorbell MSI-X interrupts = control plane** (new-request, grant-ready,
  release wakeups). Lets the manager sleep at 0% CPU and wake in ~µs; scales to N
  clients without O(N) polling. Interrupts are **not** in the data path.
- **Shared BAR2 = data plane** (carries `gpu_request_t` / `gpu_grant_t` and bulk
  data). Polling/shared-mem wins for throughput, so we don't interrupt per byte.

**Runtime kick path:** guest writes Doorbell (BAR0) → VMEXIT → QEMU ioeventfd →
eventfd → KVM irqfd → MSI-X injected into target guest. The `ivshmem-server` is a
**setup-time broker only** (hands out the shmem fd + eventfds + peer IDs via
SCM_RIGHTS at connect), then idles — it is **not** on the runtime path.

**SEV-SNP specifics:**
- BAR2 is MMIO → `ioremap` maps it with the C-bit clear (shared) automatically.
  **No PVALIDATE needed.**
- Userspace mmap of BAR2 uses `pgprot_decrypted(pgprot_noncached(...))`.
- Doorbell MMIO write traps as #VC/VMGEXIT (slightly pricier than bare metal;
  negligible at control-plane frequency).
- **Fallback:** if MSI-X injection into the SNP guest is troublesome, a
  plain + poll path is built in (driver sets `nvectors = 0` and still works).

### Register / doorbell facts
- BAR0 regs: `0x00` IntrMask, `0x04` IntrStatus, `0x08` IVPosition (RO peer id,
  `-1` if plain), `0x0C` Doorbell (WO): value = `(peer_id << 16) | vector`.
- Ring helper: `writel((peer << 16) | vector, regs + 0x0C)`.

### MSI-X vector semantics
- vec0 = `NEW_REQUEST`  (client → manager)
- vec1 = `GRANT_READY`  (manager → client)
- vec2 = `RELEASE`      (client → manager)
- `IVSHMEM_NUM_VECTORS = 4`; manager peer id = 0 (`SEV_GPU_MANAGER_PEER_ID`).

---

## 3. UNIFIED MODULE (key design decision)

**One** module `sev_gpu_manager.ko` serves **both** roles, selected by a module
parameter:

- `manager=1` (default) → GPU-owning VM: initializes the BAR2 header, runs the
  scheduler, grants GPU time.
- `manager=0` → client VM: reads the layout, writes requests, blocks for grants.

The client side is **fully implemented in the same `.ko`**:
- `probe` reads the layout the manager published (retries on ioctl if the manager
  hasn't initialized yet).
- `REQUEST_GPU` writes the client's per-VM request slot and rings the manager's
  `NEW_REQUEST` doorbell.
- `WAIT_GRANT` blocks on `grant_wq`, woken by the `GRANT_READY` IRQ.
- `RELEASE_GPU` rings the `RELEASE` doorbell.

So **no separate client codebase** — build the same module on every VM and load it
with the appropriate role.

---

## 4. Current status

### Session log 2026-06-23 — GPU bring-up on the manager CVM (real GPU)

**What we did today:**
- **Verified `nvidia.ko` actually builds + links** against the guest kernel
  (`6.16.0-snp-guest-038d61fd6422`) with all our RM-core CC functions
  (`rm_sev_gpu_get_kmb / alloc_cc_channel / free_cc_channel / ce_submit`).
- **Fixed modpost `undefined!` linker errors** — `nv-kernel.o` links with
  `--gc-sections`, so any RM→kernel-open boundary symbol must be force-retained.
  Added 4 `--undefined=` lines to
  `src/nvidia/exports_link_command.txt` (`rm_sev_gpu_get_channel_kmb`,
  `rm_sev_gpu_alloc_cc_channel`, `rm_sev_gpu_free_cc_channel`,
  `rm_sev_gpu_ce_submit`). **Verified on guest:** all four exports show as
  `T … [nvidia]` in `/proc/kallsyms`.
- **Fixed the `symbol_get` GPL failures** — `__symbol_get()` only resolves
  `EXPORT_SYMBOL_GPL`. dmesg showed *"failing symbol_get of non-GPLONLY symbol
  sev_gpu_rm_…"* so every manager bind fell back to "absent". Changed all **8**
  boundary exports in `kernel-open/nvidia/nv.c` from `EXPORT_SYMBOL` →
  `EXPORT_SYMBOL_GPL` (`sev_gpu_register/unregister_rm_forwarder`,
  `sev_gpu_rm_replay[_teardown]`, `sev_gpu_rm_get_kmb`,
  `sev_gpu_rm_alloc/free_cc_channel`, `sev_gpu_rm_ce_submit`). nvidia.ko is
  `Dual MIT/GPL` so GPL export is allowed. **Edit is on the host tree only —
  not yet synced/rebuilt/loaded on the guest.**
- **Added host-runnable tests + cross-VM orchestration:**
  - `userspace/cc_crypt_selftest.c` — CC crypto known-answer test (mirrors the
    kernel CRYPT ioctl contract: `gcm_iv = counterLE ^ ivMask`, AES-256-GCM, no
    AAD, 16-byte tag). **PASSES** on host. Wired into `userspace/Makefile`.
  - `userspace/kmb_test.c` — new `provision [keyspace] [count]` command
    (PROVISION_POOL → allocates CC channels via the bound RM allocator).
  - `test.sh` — new subcommands: `crypt-test`, `nvidia build|load|unload|status`,
    `gpu-provision [keyspace] [count]`, and `gpu-setup manager|client [vm_id]
    [keyspace] [count]` (full per-VM build/load/provision ladder).
- **On-hardware bring-up loop on the manager-vm** (real GPU `10de:2bb5`,
  GB202 / RTX PRO 6000 Blackwell Server Edition, passthrough): patched nvidia.ko
  loads; GPU bound to `nvidia`; CC exports present.

**CURRENT BLOCKER — GPU won't attach (`nvidia-smi`: "No devices were found"):**
- dmesg root cause: `confComputeConstructEngine_IMPL: GPU confidential compute
  capability is not enabled` → `NV_ASSERT_OR_RETURN(0, NV_ERR_INVALID_REQUEST)`
  → `osInitNvMapping: Cannot attach gpu` → `RmInitAdapter failed! (0x22:0x3f:894)`.
- **Diagnosis (from `conf_compute.c`):** the assert fires when
  `bOsCCEnabled(=SEV-SNP guest, TRUE) && !gpuIsCCEnabledInHw(GPU CC mode OFF) &&
  !gpuIsProtectedPcieEnabledInHw`. The GPU **is** CC-capable
  (`PDB_PROP_GPU_CC_FEATURE_CAPABLE`), so RM refuses to attach a CC-capable GPU
  inside a confidential VM when the GPU's hardware CC mode is not turned on
  (insecure mismatch). **This is a GPU provisioning step, not a code bug.**
- **Fix:** enable GPU CC mode **on the host, before passthrough**, with NVIDIA's
  GPU admin tool (`nvidia_gpu_tools.py --set-cc-mode=on --reset-after-cc-mode-switch`
  from github.com/NVIDIA/gpu-admin-tools; the tool is **not** in this workspace).
  `gpuIsCCEnabledInHw_HAL()` reads the persistent CC-mode bit that tool sets.

**Next steps (in order):**
1. Host: set GPU CC mode `on` (or `devtools`) + reset, re-bind vfio-pci, boot the
   manager VM. Confirm with `nvidia_gpu_tools.py --query-cc-mode`.
2. Guest: sync the `nv.c` GPL fix, rebuild + reload nvidia.ko, confirm
   `nvidia-smi` lists the GPU, reload manager, confirm dmesg shows "bound
   nvidia.ko …" (not "absent"), then `./test.sh gpu-provision 0 1`.
3. **Stage 2 — `SUBMIT_COPY` end-to-end** (CPU-encrypt → GPU-CE-decrypt → verify):
   needs buffer physaddr plumbing (sysmem bounce phys + VRAM phys + 16B-aligned
   authTag/IV). Currently `src/dst/authTag/iv` offsets pass STRAIGHT THROUGH to
   `rm_sev_gpu_ce_submit` as physical addresses — no translation layer yet.

---

### GPU test runbook (manager CVM + client CVM)

> Three machines: **HOST** (this workspace, SNP host) · **manager-vm** (SNP guest,
> owns the GPU) · **client CVM** (SNP guest, no GPU — Option A). Source reaches
> guests via the read-only virtio-9p share (`sev_src`), copied to `~/sev-gpu`.
> **NEVER insmod on the host. Manager VM must launch FIRST (IVPosition 0).**

**Step 0 — HOST: enable GPU CC mode + bring up shared memory (one-time / per boot)**
```bash
# (a) GPU CC mode ON — manager VM must be shut down so the host owns the GPU.
lspci -nnd 10de:                       # find the GPU BDF (10de:2bb5)
cd gpu-admin-tools                     # github.com/NVIDIA/gpu-admin-tools
sudo python3 ./nvidia_gpu_tools.py --gpu-bdf <BDF> \
     --set-cc-mode=on --reset-after-cc-mode-switch
sudo python3 ./nvidia_gpu_tools.py --gpu-bdf <BDF> --query-cc-mode   # -> on

# (b) ivshmem shared memory: control region (doorbell) + per-VM data regions.
cd ~/workspace/sev-changes
./host/ivshmem_server.sh               # leave running: broker for shmem fd + eventfds
./host/qemu_ivshmem_args.sh --setup    # truncate the per-VM data region backing files
```

**Step 1 — HOST: launch the VMs (manager FIRST)**
```bash
# manager owns the GPU -> use the GPU passthrough launcher; it also attaches the
# control ivshmem-doorbell + ALL data regions. (See host/qemu_ivshmem_args.sh.)
~/ManagerCVM/launch-cvm-gpu.sh         # vfio-pci must have released the GPU first
# then each client (attaches control doorbell + ONLY its own data region):
~/CVM/launch-cvm.sh
```

**Step 2 — manager-vm: drivers, channels, provisioning**
```bash
cd ~ && /mnt/sev-src/guest_setup.sh manager   # or: mount 9p + copy to ~/sev-gpu
cd ~/sev-gpu
./test.sh sync                         # pull latest host edits (incl. nv.c GPL fix)

# nvidia first (CC mode now ON -> RM attaches), then our manager module.
sudo rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia 2>/dev/null
./test.sh nvidia build                 # make modules SYSSRC=/lib/modules/$(uname -r)/build
./test.sh nvidia load
nvidia-smi                             # MUST now list the GPU
sudo grep ' T sev_gpu_rm' /proc/kallsyms   # GPL exports present

./test.sh reload manager               # symbol_get binds the RM CC funcs at init
sudo dmesg | grep sev_gpu | tail       # expect "bound nvidia.ko …" (not "absent")
./test.sh gpu-provision 0 1            # PROVISION_POOL -> "provisioned 1/1 CC channel(s)"

# one-shot equivalent of the above:  ./test.sh gpu-setup manager 0 0 1
```

**Step 3 — client CVM: driver (no GPU) + crypto**
```bash
cd ~ && /mnt/sev-src/guest_setup.sh client
cd ~/sev-gpu
./test.sh reload client                # manager=0; client does SOFTWARE crypto (Option A)
# or one-shot:  ./test.sh gpu-setup client <vm_id>
```

**Step 4 — handshake (manager FIRST at each step) + data plane**
```bash
# Key broker / KMB delivery over the TLS-over-ivshmem tunnel (manager -> client):
# manager:
./test.sh keybroker                    # serve channel KMB to the assigned client
./test.sh kmb-test manager
# client:
./test.sh kmb-test client
./test.sh kmb-test crypt <channel>     # client AES-256-GCM into its bounce buffer

# Host-side sanity (any machine, no GPU): CC crypto known-answer test
./test.sh crypt-test                   # cc_crypt_selftest -> PASS

# Stage 2 (once buffer physaddr plumbing lands): manager submits the CE secure
# copy that DMAs the client's ciphertext from ivshmem and decrypts into VRAM.
```

---

**DONE (compiles clean on the dev PC, kernel 6.8):**
- `kernel/sev_gpu_manager.h` — structs (`gpu_request_t`, `gpu_grant_t`,
  `sev_gpu_shmem_header_t`), ivshmem register offsets, vector ids, ioctls
  (REGISTER_VM, GET_SHMEM, REQUEST_GPU, GET_GRANT, RELEASE_GPU, **GET_ROLE**,
  **WAIT_GRANT**).
- `kernel/sev_gpu_manager.c` — unified ivshmem PCI driver (Phases A+B+C):
  probe/remove, BAR0/BAR2 mapping, MSI-X setup with poll fallback, doorbell ring,
  IRQ handler, manager greedy scheduler, char dev + mmap (decrypted+noncached),
  full ioctl set.
- `userspace/sev_gpu_client.{c,h}` — blocking `WAIT_GRANT` ioctl (no more
  polling), `GET_ROLE` on open, per-VM request/grant slots.
- `userspace/example_vm_app.c` — role-aware, accepts optional `vm_id` / name argv.
- `userspace/Makefile`, `test.sh` (role-aware `load`/`test`, `lspci 1af4:1110`
  status), `host/ivshmem_server.sh`, `host/qemu_ivshmem_args.sh`.

**PENDING (needs the real host + SNP guest — not doable on the dev PC):**
- Build the module on the SNP guest (kernel 6.16) — all APIs used are present in
  6.16.
- Verification ladder V2–V6 (below).
- **Phase D** (GPU work-forwarding) — intentionally deferred until comms verifies
  on hardware. Do **not** build it on an unverified comms layer.

**Three separate machines (do not conflate):**
- **Dev PC** (original workspace): compile sanity only. Kernel 6.8, gcc-12.
- **Host**: separate physical machine running QEMU + `ivshmem-server`. The
  `host/*.sh` scripts are deliverables to copy/run *here*.
- **manager-vm**: the SEV-SNP guest. Ubuntu 24.04.4,
  kernel `6.16.0-snp-guest-038d61fd6422`, x86_64. The module runs here.

---

## 5. Deployment

**On each VM (manager-vm and each client VM):**
```bash
cd sev-changes/kernel && make        # builds against /lib/modules/$(uname -r)/build
cd ../userspace && make
```

**On the host (separate machine):**
```bash
sev-changes/host/ivshmem_server.sh   # starts ivshmem-server, prints the QEMU args
# Add those args (see host/qemu_ivshmem_args.sh) to EACH VM's QEMU command line:
#   -chardev socket,path=/tmp/ivshmem_socket,id=ivshmem
#   -device  ivshmem-doorbell,chardev=ivshmem,vectors=4
# The manager VM must connect FIRST so it gets IVPosition 0.
# The manager VM also has the GPU via: -device vfio-pci,host=$GPU
```
Env overrides for the server: `IVSHMEM_SOCK`, `IVSHMEM_SHM`, `IVSHMEM_SIZE`
(default 256M), `IVSHMEM_VECTORS` (default 4), `IVSHMEM_SERVER` (binary path).

**Load (role-aware) on each VM:**
```bash
# manager-vm (owns GPU):
./test.sh load manager
# each client VM:
./test.sh load client
./test.sh status        # confirms lspci 1af4:1110 + /dev/sev_gpu_manager
./test.sh test client   # request -> wait_grant -> release end-to-end
```

---

## 6. Verification ladder

Validate the driver/protocol on **ordinary QEMU VMs first** (non-confidential) to
isolate ivshmem bugs from SEV bugs, then port to SNP.

- **V1 (build):** module compiles; `modinfo` shows `1af4:1110` in the alias. ✅ on
  dev PC 6.8; redo on the SNP guest 6.16.
- **V2 (single-VM bind):** one QEMU with ivshmem-doorbell + ivshmem-server running;
  `lspci -d 1af4:1110` present; dmesg shows probe, BAR2 size, ivposition.
- **V3 (cross-VM RAM):** two VMs; manager writes the header magic to BAR2; client
  reads the **same** magic → proves shared RAM.
- **V4 (doorbell):** client `REQUEST_GPU` → manager ISR logs; manager grant →
  client ISR logs.
- **V5 (end-to-end):** client `REQUEST_GPU` → manager scheduler → grant → client
  `WAIT_GRANT` returns via interrupt (no poll). Multi-client round-robin.
- **V6 (SEV port):** repeat V3–V5 on SEV-SNP CVMs; confirm BAR2 shared mapping +
  MSI-X injection.

---

## 7. Phase D — GPU access (deferred until comms verified)

Two options (Option 1 recommended):
1. **Work forwarding / API remoting** — clients ship GPU work + data to the
   manager via per-client BAR2 buffers; the manager runs them on the
   passed-through GPU and writes results back. Matches the `tgpu-llm.c`
   `data_holder` ↔ `orchestrator` flow. **Recommended.**
2. **Time-multiplexed VFIO handoff** — references `tgpu-kernel-patch` VFIO MUX.
   Complex.

Default to Option 1; revisit after comms (V5/V6) passes.

---

## 8. Risks / watch-items

- MSI-X injection into the SNP guest may need extra wiring → plain + poll fallback
  is already in the driver (`nvectors = 0` path).
- BAR2 cache attribute starts UC (correct/shared); optimize WC/WB later.
- Doorbell-mode shared memory comes from `ivshmem-server`'s shm (size via `-l`),
  not a `memory-backend-file`.
- Manager **must** be ivshmem peer 0; ensure it connects first.
- Cross-VM testing needs a 2-VM host (the dev PC is itself a single CVM).
- Workspace terminal note: the original dev PC used `nu` shell where `&&`
  chaining fails — use `;`. (The host/guest shells are bash.)

---

## 9. File map (under `sev-changes/`)

| Path | Purpose |
|------|---------|
| `kernel/sev_gpu_manager.c` | Unified ivshmem PCI driver (manager + client) |
| `kernel/sev_gpu_manager.h` | Shared structs, ivshmem reg offsets, vectors, ioctls |
| `kernel/sev_gpu_manager.c.phase1.bak` | Old vmalloc Phase-1 backup (superseded) |
| `userspace/sev_gpu_client.{c,h}` | Client library (same char dev for both roles) |
| `userspace/example_vm_app.c` | Role-aware demo: open→request→wait_grant→release |
| `userspace/Makefile` | Builds `libsev_gpu.so` + `example_vm_app` |
| `host/ivshmem_server.sh` | Start `ivshmem-server` (run on the HOST) |
| `host/qemu_ivshmem_args.sh` | QEMU `-device` args to inject (HOST) |
| `test.sh` | load/unload/test/status/rebuild/clean (run on each VM) |

---

## 10. Why the previous (Phase-1) approach was scrapped

Phase-1 backed the shared region with `vmalloc`. That memory is **guest-local** —
each CVM is a separate VM with its own kernel, so vmalloc cannot be shared across
VMs. It only appeared to work on the non-confidential dev PC because everything
was locally accessible. Cross-VM shared memory fundamentally requires the
hypervisor to map the same physical RAM into both guests; ivshmem provides exactly
that as a PCI BAR, and BAR-as-MMIO avoids PVALIDATE under SEV.

---

## 11. Manager-allocated compute channels (Arch B — LOCKED)

**Directive (LOCKED):** the **manager allocates EVERY GPU channel** and assigns
them to clients from a pool; clients **never** allocate their own channels. The
manager delivers each assigned channel's **KMB**, scopes **work-submit** to the
VM that owns the channel, and is the **sole doorbell-ringer**. Zero-copy
(USERD/pushbuffer in shared ivshmem) is maintained except where it provably
can't be. Channel *reassignment* among clients is deferred.

**Why staged, not one drop:** a correct in-kernel **compute (GR) GPFIFO channel**
allocator has no in-tree precedent (CeUtils/OBJCHANNEL is CE/SEC2-only; UVM's
`nvGpuOpsChannelAllocate` is entangled with UVM-private VA machinery and never
does GR). It must be built and **VM-verified layer-by-layer** — a blind ~600-line
RM drop is exactly the orphan-trap mistake this project already hit. Ground every
RM call in real in-tree APIs.

### Grounding (verified in this tree)
- **Base tree helper EXISTS:** `rmapiutilAllocClientAndDeviceHandles(pRmApi, pGpu,
  &hClient, &hDevice, &hSubDevice)` →
  `NV01_ROOT` + `NV01_DEVICE_0` (`NV0080_ALLOC_PARAMETERS`) + `NV20_SUBDEVICE_0`
  (`NV2080_ALLOC_PARAMETERS`). Teardown: `rmapiutilFreeClientAndDeviceHandles`.
  Uses `pRmApi->AllocWithHandle` + `serverutilGenResourceHandle`; no manual GPU
  lock (the `RMAPI_GPU_LOCK_INTERNAL` interface locks per call).
- **Authoritative full-channel reference:** `nv_gpu_ops.c` `channelAllocate`
  (~L5737-6130), VASpace `nvGpuOpsAddressSpaceCreate` (L2511, `FERMI_VASPACE_A`),
  TSG `nvGpuOpsTsgAllocate` (L6360, `KEPLER_CHANNEL_GROUP_A`).
- **Param structs (`nvos.h`):** `NV_VASPACE_ALLOCATION_PARAMETERS{index=
  NV_VASPACE_ALLOCATION_INDEX_GPU_NEW, flags, vaBase, vaSize, ...}`;
  `NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS{hObjectError, hVASpace, engineType, ...}`.
- **Classes/engine:** `FERMI_VASPACE_A` 0x90f1 (`class/cl90f1.h`),
  `KEPLER_CHANNEL_GROUP_A` 0xa06c (`class/cla06c.h`),
  `BLACKWELL_CHANNEL_GPFIFO_A` 0xc96f (`class/clc96f.h`),
  `BLACKWELL_COMPUTE_A` 0xcdc0 (`class/clcdc0.h`),
  `NV2080_ENGINE_TYPE_GR0` 0x1 (`class/cl2080_notification.h`).
- **CC + USERD:** channel `flags` need `NVOS04_FLAGS_CC_SECURE=_TRUE` (required for
  KMB); USERD may live in unprotected sysmem under HCC. Back USERD + pushbuffer
  with shared ivshmem via the proven `rm_sev_gpu_ce_submit` pattern
  (`memdescCreate(ADDR_SYSMEM, MEMDESC_FLAGS_ALLOC_IN_UNPROTECTED_MEMORY)` +
  `memdescDescribe(phys)`) wrapped as an `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` Memory
  handle and passed as `hUserdMemory`; GPFIFO needs a GPU VA (`pRmApi->Map`).
- **KMB:** `NVC56F_CTRL_CMD_GET_KMB` 0xc56f010b via `rm_sev_gpu_get_channel_kmb`.
  Whether 0xc96f shares that control (no `ctrlc96f.h` in this tree) is a VM-verify.

### Staged build ladder (VM-verify each rung)
- **L1 — base tree (DONE, built; VM smoke-test pending).** nvidia.ko
  `rm_sev_gpu_alloc_compute_channel`: client/device/subdevice (helper) + VASpace
  (`FERMI_VASPACE_A`) + TSG (`KEPLER_CHANNEL_GROUP_A`,
  `engineType=NV2080_ENGINE_TYPE_GR0`), per-step dmesg + full teardown
  (`rm_sev_gpu_free_compute_channel`, freeing the client root cascades the tree).
  Exported wrappers `sev_gpu_rm_alloc_compute_channel` /
  `…_free_compute_channel` (`EXPORT_SYMBOL_GPL` in `nv.c`), protos in both `nv.h`,
  `--undefined=` lines in `exports_link_command.txt` (defined BEFORE the export
  line — orphan-trap guard; `LD nvidia.ko` passed). Manager binds
  `compute_alloc_fn` / `compute_free_fn` in `kmb_manager_bind_nvidia`; module
  param `compute_selftest=N` alloc+log+frees N channel trees at bind
  (`sev_gpu_compute_selftest`). Both modules build clean. **Verify on VM:** load
  manager with `compute_selftest=1`; dmesg should show the GR0 TSG built + freed.
- **L2 — channel + compute object (DONE, built; VM smoke-test pending).**
  `rm_sev_gpu_alloc_compute_channel` extended on top of the L1 tree: a physical
  sysmem GPFIFO backing (`NV01_MEMORY_SYSTEM`, unprotected), a virtual GPFIFO
  buffer (`NV50_MEMORY_VIRTUAL` on the channel VASpace), an unprotected-sysmem
  USERD (`sizeof(KeplerAControlGPFifo)`), the GR0 GPFIFO channel itself
  (`kfifoGetChannelClassId`, `CC_SECURE`, parented to the L1 TSG with
  `hVASpace=NV01_NULL_OBJECT`, `gpFifoOffset=0` — created, not yet submitted to),
  and a `BLACKWELL_COMPUTE_A` object. `*phChannel` now returns the real channel
  handle; per-step dmesg; teardown unchanged (client-free cascades). **Lock fix:**
  `RMAPI_GPU_LOCK_INTERNAL` requires the caller to hold BOTH the API and GPU locks
  (`rmapi.c` `bApiLockInternal=TRUE, bGpuLockInternal=TRUE`); alloc+free now take
  the GPU lock (`rmGpuLocksAcquire`, mirroring the in-file CC allocator) instead of
  the API lock alone. Both modules build clean. **Verify on VM:** load manager with
  `compute_selftest=1`; dmesg should show the full channel (VASpace/TSG/pushbuffers/
  USERD/GPFIFO-channel/BLACKWELL_COMPUTE_A) built + freed, with a non-zero hChannel.
- **L3 — schedulable channel + shared-mem backing + manager pool (split into
  sub-rungs).**
  - **L3.1 — schedulable channel (DONE, built; VM smoke-test pending).** In
    `rm_sev_gpu_alloc_compute_channel`: map the physical GPFIFO backing into the
    channel VASpace (`pRmApi->Map` / `NVOS46_PARAMETERS` `hDma=hPbVirt`,
    `hMemory=hPbPhys` → `dmaOffset`), allocate the channel with that real
    `gpFifoOffset`, and schedule it (`pRmApi->Control` /
    `NVA06F_CTRL_CMD_GPFIFO_SCHEDULE` `{bEnable=NV_TRUE}`). Scheduling a CC_SECURE
    channel is KMB-independent (submit-time), matching `nv_gpu_ops` `engineAllocate`.
    USERD/GPFIFO still RM-local sysmem. nvidia.ko builds clean. **Verify on VM:**
    `compute_selftest=1` dmesg should show `gpFifoVa=…` + `channel scheduled — L3.1
    runnable channel complete`.
  - **L3.2 — shared-mem backing (DONE, built; VM smoke-test pending).** User chose
    **allocate-on-assign** (the `ce_submit` evidence shows the GPU only DMAs to a
    client's private DATA region, so a channel's USERD/GPFIFO must live there).
    `rm_sev_gpu_alloc_compute_channel` now takes `userdGpa`/`gpFifoGpa`; when set,
    USERD and the GPFIFO ring are backed by that shared memory via
    `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` (`NVOS32_DESCRIPTOR_TYPE_OS_PHYS_ADDR`,
    kernel-only, contiguous/page-aligned/unprotected). With 0/0 they fall back to
    RM-local sysmem. Export wiring (`nv.c`, both `nv.h`) + manager typedef/extern
    updated; the selftest exercises the shared path from `data_devs[0]` when a DATA
    region is attached. Both modules build clean.
  - **L3.3 — manager pool + assignment (DONE, built; VM smoke-test pending).**
    The per-VM assignment registry (`assign_state.a[vm][8]`) is itself the pool of
    slots; a `kind` field tags CE vs COMPUTE. `sev_gpu_assign_compute_channel`
    claims a free slot, carves that slot's two pages (USERD + GPFIFO) from the
    assignee's DATA region just below the RPC staging window
    (`sev_gpu_compute_carve`), builds the channel via `compute_alloc_fn`, and
    records the handles + region-relative offsets. Exposed as the manager-only
    `SEV_GPU_IOC_ASSIGN_COMPUTE` ioctl; `compute_assignments_drain` frees them at
    unbind. Both modules build clean.
- **L4 — KMB fetch (DONE, built; VM smoke-test pending).** `sev_gpu_assign_compute_channel`
  now pulls the channel's real CC_KMB via the existing `kmb_fetch_fn` (`GET_KMB`),
  which `KernelChannel` exports for any CC-secure GPFIFO channel — no nvidia.ko
  change. The generic `SEND_KMB`/`RECV_KMB` path then seals + delivers it. Builds
  clean.
- **Lock submission (DONE, built; VM smoke-test pending).** Flipped
  `enforce_channel_ownership` default 0→1 so each VM may only submit on a channel
  in its `assign_state` (CE via `ASSIGN_CHANNEL`/handshake, compute via
  `ASSIGN_COMPUTE`). Legacy replay-allocated channels are now refused `-EACCES`;
  set `enforce_channel_ownership=0` at load for legacy bring-up. Builds clean.

### Done already (this stream)
- Ownership-scoped submission in `sev_gpu_manager.c`: `sev_gpu_vm_owns_channel`
  (scans `assign_state`) gates `sev_gpu_do_submit_work`; module param
  `enforce_channel_ownership` (default 0 during bring-up → flip to 1 at L4).
  Builds clean.
- **L1 base tree (built):** nvidia.ko in-kernel GR allocator/free + full export
  wiring + manager bind + `compute_selftest` smoke harness. Both `nvidia.ko` and
  `sev_gpu_manager.ko` build clean. Awaiting on-VM `compute_selftest=1` dmesg.
- **L2 channel + compute object (built):** GPFIFO channel (CC_SECURE, in the L1
  TSG) + `BLACKWELL_COMPUTE_A`, RM-allocated unprotected-sysmem USERD/pushbuffers.
  Corrected the RM lock model to hold both API+GPU locks. `nvidia.ko` builds clean
  (`LD nvidia.ko` passed). Awaiting on-VM `compute_selftest=1` dmesg.
- **L3.1 schedulable channel (built):** GPFIFO pushbuffer DMA-mapped to a GPU VA
  (`pRmApi->Map`), channel allocated with the real `gpFifoOffset`, and scheduled
  (`pRmApi->Control` / `NVA06F_CTRL_CMD_GPFIFO_SCHEDULE`). KMB-independent (matches
  `nv_gpu_ops` `engineAllocate`). `nvidia.ko` builds clean. Awaiting on-VM
  `compute_selftest=1` dmesg.
- **L3.2 shared-mem backing (built):** allocate-on-assign chosen; USERD/GPFIFO
  backed from the assigned client's DATA region via `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR`
  (OS_PHYS_ADDR), with RM-local fallback. Signature + export wiring + manager
  selftest updated. Both modules build clean.
- **L3.3 manager pool + assign (built):** per-VM `assign_state` is the slot pool
  (`kind` tags CE vs COMPUTE); `sev_gpu_assign_compute_channel` carves the slot's
  USERD/GPFIFO from the assignee's DATA region (`sev_gpu_compute_carve`, below the
  RPC staging window) and builds the channel on assign. Manager-only
  `SEV_GPU_IOC_ASSIGN_COMPUTE` ioctl + `compute_assignments_drain` teardown. Both
  modules build clean.
- **L4 KMB fetch (built):** the compute assign now fetches the channel's real
  CC_KMB via `kmb_fetch_fn` (`GET_KMB`, exported on the `KernelChannel` base class
  and gated only on `bCCSecureChannel` — works on `BLACKWELL_CHANNEL_GPFIFO_A`), so
  no nvidia.ko change was needed; on fetch failure the half-built channel is freed.
  The generic `SEND_KMB`/`RECV_KMB` seal+deliver path carries it. `sev_gpu_manager.ko`
  builds clean.
- **Submission lock (built):** `enforce_channel_ownership` now defaults to 1, so a
  VM may only ring a doorbell on a channel in its `assign_state`; unassigned
  (legacy replay) channels are refused `-EACCES`. Load with
  `enforce_channel_ownership=0` for legacy bring-up. Builds clean; VM-verify the
  full assign\u2192deliver\u2192submit flow next.
