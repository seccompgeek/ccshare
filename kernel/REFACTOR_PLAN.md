# sev_gpu module — separation plan (one module, role-branched)

Current: one 7,641-line `sev_gpu_manager.c` + `sev_gpu_manager.h` (844) + `sev_gpu_rpc.h` (197).
Goal: separate by concern into files, keep ONE buildable module, and put a clean
**transport boundary** in place so the ivshmem layer can later be swapped for the
sev-channel (ioeventfd/irqfd) transport with minimal churn.

Principle: don't fight the existing clustering — the code already groups cleanly.
Role stays runtime-selected (control vs data device, manager vs client), NOT two
modules.

## Target files

### Core / shared
- **sev_gpu_main.c** — module init/exit, PCI probe/remove, role detection,
  `pdev_is_control`, char-dev registration dispatch. (~540 lines: 7105–7641 +
  init glue)
- **sev_gpu_regions.c/.h** — BAR2 geometry: `rpc_staging_base`,
  `compute_reserve_base`, `compute_doorbell_off`, `osdesc_reserve_base`,
  `wlc_lcic_reserve_base`, `sev_gpu_compute_carve`, `sev_gpu_client_reserve_band`.
  (494–782) Both roles include this; offsets MUST agree — single source.
- **sev_gpu.h** — the shared internal header (structs `sev_gpu_dev`,
  `sev_gpu_data_dev`, globals, enums). Split from current manager.h.

### Transport boundary (the swap point)
- **sev_gpu_transport.h** — ABSTRACT interface. Everything above it is
  transport-agnostic:
    - `sev_xport_ring(dev, peer, vector)`        (was `ivshmem_ring`)
    - `sev_xport_wake_manager()`                  (was `rpc_wake_manager`)
    - `sev_xport_ctrl_mailbox(vm)` / `req_slot` / `grant_slot` / `kmb_mailbox`
      / `hs_ctrl_mailbox` / `rpc_ctrl_mailbox`   (region accessors)
    - `sev_xport_irq_setup/free/mask/unmask/rearm`
    - `sev_xport_peer(dev)`                        (was `sev_gpu_manager_peer`,
      IVPosition-derived; becomes static id under channel)
    - layout publish/read (`manager_init_layout` / `client_read_layout`)
- **sev_gpu_xport_ivshmem.c** — CURRENT implementation of the above, holding all
  `d->regs`, `ivposition`, `IVSHMEM_*`, doorbell-vector logic. (1040–1320 + the
  irq/mask/rearm + layout bits scattered 1040–1200, 2372–2470)
- **sev_gpu_xport_channel.c** — LATER. Implements the same header on the
  sev-channel device (ioeventfd doorbell + irqfd completion, per-client fds).
  Drop-in: change which .c compiles.

### Communication / crypto (transport-agnostic, sits ON the boundary)
- **sev_gpu_comm.c/.h** — RPC framing + nested layout policy (`rpc_nested`,
  `rpc_ctrl_policy`, `rpc_nested_layout`, `bv_fields` …). (782–1005)
- **sev_gpu_crypto.c/.h** — AEAD, SHA256, HMAC, HKDF, ECDHE
  (`sev_gpu_aead`, `_sha256`, `_hmac_sha256`, `_hkdf_expand32`, `_ecdhe_*`,
  `_hs_derive`, `_get_psk`). (2829–3808) Pure crypto, no transport, no role.
- **sev_gpu_handshake.c** — ECDHE-PSK exchange + comm-key commit/wait
  (`hs_ctrl_mailbox`, `commit_comm_key`, `wait_comm_key`, `hs_service_slot`,
  `hs_wait_reply`, `hs_client_run`, `hs_client_maybe_run`, `hs_work`). (3543–4140)
  Uses transport mailbox accessors + crypto. This is the **KMB/comm-key path** you
  want isolated for the channel move.
- **sev_gpu_kmb.c** — KMB seal/install/send/recv + AAD + fp
  (`kmb_mailbox`, `kmb_fp`, `send_kmb`, `recv_kmb`, `kmb_seal_impl`,
  `kmb_install_impl`). (2811–2937, 3213–3526, 4138–4390)

### Manager-only
- **sev_gpu_manager_sched.c** — scan/grant, sched work, client channel setup,
  register_vm, note_active/release (`scan_and_grant`, `sched_work`,
  `manager_setup_client_channels`, `note_client_active`, `register_vm` …).
  (1208–1482)
- **sev_gpu_manager_exec.c** — the GPU execution path: `do_ce_copy`,
  `vm_owns_channel`, `do_submit_work`, `assign_channel`,
  `assign_compute_channel`, `rpc_service`, `copy_service`, `rpc_service_slot`,
  `rpc_thread_fn`. (1482–1600, 1716–1869, 2205–2332, 2937–3213, 6415–6850)
- **sev_gpu_manager_nvidia.c** — nvidia.ko symbol binding for the manager side
  (`rpc_manager_bind/unbind_nvidia`, `kmb_manager_bind/unbind`,
  `sev_gpu_shadow_db_impl`, `osdesc_carve/reset_impl`, `shared_phys_to_va_impl`,
  `compute_selftest`, pool drains). (5831–6415)
- **sev_gpu_bringup.c** — autonomous bring-up doorbell watcher
  (`bringup_watch`, `bringup_arm/disarm/poll`, usermode hrtimer). (1869–2205,
  6941–6998)

### Client-only
- **sev_gpu_client_rm.c** — `sev_gpu_rm_forward` (the big RM interceptor),
  `rpc_client_bind/unbind_nvidia`, `rpc_client_call`. (5298–5831, 7030–7105,
  2465–2564)
- **sev_gpu_client_mmap.c** — mmap redirect + shadow doorbell consume
  (`mmap_redirect_impl`, `doorbell_mmap_pfn`, `mmap_client_bind/unbind_nvidia`).
  (6849–6941, 6998–7030)
- **sev_gpu_client_gpu.c** — synthetic GPU attach/detach + desc publish
  (`publish_gpu_desc`, `client_attach_gpu`, `client_detach_gpu`). (7105–7219)

### ioctl + chardev (dispatches to role)
- **sev_gpu_ioctl.c** — `sev_gpu_ioctl` (big switch), `rpc_client_call`,
  `request_copy`, `request_submit_work`, `flush_channels`, fops, chardev setup,
  data-dev fops/ioctl/mmap. (2442–2811, 4390–5300)

## Order of operations (safe, incremental)

1. **Header split first** — carve `sev_gpu.h`, `sev_gpu_regions.h`,
   `sev_gpu_transport.h`, `sev_gpu_comm.h`, `sev_gpu_crypto.h`. No code moves yet;
   just declarations. Module still builds as one .c including them.
2. **Extract leaf clusters** with no back-dependencies: crypto, regions, comm
   framing. These move cleanly (pure functions).
3. **Define the transport interface** and route ALL current `ivshmem_ring`,
   `d->regs`, `ivposition`, mailbox-accessor calls through `sev_xport_*`. Move the
   ivshmem bodies into `sev_gpu_xport_ivshmem.c`. This is the key step — after it,
   nothing outside that file mentions ivshmem.
4. **Split manager vs client** clusters into their files.
5. **ioctl/chardev** last (it references everything).
6. Build after each step (Makefile `sev_gpu-objs := ...` list grows).

## The channel-swap payoff

After step 3, moving to the sev-channel transport = write
`sev_gpu_xport_channel.c` implementing `sev_gpu_transport.h` on the channel
device (BAR2 shared file, ioeventfd doorbell, irqfd completion, per-client fd
handoff), and swap one line in the Makefile. The comm/crypto/handshake/KMB layers
don't change — they only call `sev_xport_*`.

## KMB over the channel (your near-term goal)

The KMB + comm-key handshake currently rides the ivshmem CONTROL region mailboxes
(`hs_ctrl_mailbox`, `kmb_mailbox`, `rpc_ctrl_mailbox` — all `void __iomem *` into
BAR2 of the control device). Under the channel, these become offsets into the
per-client BAR2 shared file. So the transport interface needs mailbox accessors
that return the right pointer per-transport:
  - ivshmem: `d->ctrl_regs_bar2 + offset`
  - channel: `client_region_base(vm) + offset`
Isolating them behind `sev_xport_*_mailbox(vm)` is what makes the KMB code move
without edits.
