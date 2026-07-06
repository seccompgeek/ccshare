# Manager-allocated compute channel (user directive, Arch B)

## Directive (verbatim intent)
Manager ALLOCATES all channels (pool assignment). Clients NEVER allocate their
own. Deliver KMBs for the assigned channel. Clients submit work THROUGH the
manager using their ASSIGNED channels; manager rings the doorbell. Zero-copy
(USERD/pushbuffer in shared ivshmem) maintained "unless where it can't be
enforced." Channel reassignment among clients = later.

## DONE this session (built clean, manager make -C kernel exit 0)
- Ownership-scoped submission in sev_gpu_manager.c:
  - module param `enforce_channel_ownership` (bool, 0644, default 0 bring-up).
  - helper `sev_gpu_vm_owns_channel(vm_id,h_client,h_channel)` scans
    assign_state.a[vm_id][] for in_use && h_client && h_channel match.
  - sev_gpu_do_submit_work: if !owns -> if enforce -> -EACCES; else pr_warn +
    allow (bring-up, replay channel not yet in registry).
  - Flip enforce_channel_ownership=1 once compute channels are pool-assigned.
- L1 base tree (BUILT clean, both nvidia.ko LD passed + manager make exit 0):
  - osapi.c: added includes nvos.h + cl90f1.h + cla06c.h + cl2080_notification.h;
    SEV_GPU_COMPUTE_POOL_MAX 32; static g_sevComputeChannels[]{inUse,hClient,
    hDevice,hSubDevice,hVASpace,hTsg,hChannel}; rm_sev_gpu_alloc_compute_channel(
    sp,flags,*phClient,*phChannel) = API lock + RMAPI_GPU_LOCK_INTERNAL +
    rmapiutilAllocClientAndDeviceHandles + AllocWithHandle(FERMI_VASPACE_A vaParams
    index=GPU_NEW) + AllocWithHandle(KEPLER_CHANNEL_GROUP_A tsgParams hVASpace+
    engineType=NV2080_ENGINE_TYPE_GR0); *phChannel=0 (L2 fills). free = find by
    hClient -> rmapiutilFreeClientAndDeviceHandles (cascades). Inserted AFTER
    rm_sev_gpu_free_cc_channel, BEFORE ce_submit comment.
  - nv.c: EXPORT_SYMBOL_GPL wrappers sev_gpu_rm_alloc_compute_channel /
    _free_compute_channel after submit_work, before nvidia_ioctl.
  - both nv.h: 2 rm_ protos + 2 sev_gpu_rm_ protos. exports_link_command.txt: 2
    --undefined lines (LD nvidia.ko passed = no orphan trap).
  - manager: typedefs sev_gpu_compute_alloc_t/_free_t; statics compute_alloc_fn/
    compute_free_fn; module param compute_selftest (uint,0444); externs+symbol_get
    in kmb_manager_bind_nvidia, symbol_put in unbind (before kmb early-return);
    sev_gpu_compute_selftest() alloc+log+free N; called in probe after
    kmb_manager_bind_nvidia() (L~4003).
- VM verify pending: load manager compute_selftest=1 -> dmesg GR0 TSG built+freed.

## DONE this session: L2 (BUILT clean, nvidia.ko LD passed, manager unchanged)
- osapi.c rm_sev_gpu_alloc_compute_channel EXTENDED with steps 4-8 after the TSG:
  (4) PB phys NV01_MEMORY_SYSTEM unprotected size=gpFifoSize hVASpace=0;
  (5) PB virt NV50_MEMORY_VIRTUAL flags=NVOS32_ALLOC_FLAGS_VIRTUAL hVASpace=hVASpace;
  (6) USERD NV01_MEMORY_SYSTEM unprotected size=sizeof(KeplerAControlGPFifo) hVASpace=0;
  (7) channel: chClass=kfifoGetChannelClassId(pGpu,GPU_GET_KERNEL_FIFO(pGpu)); chParams
      {hVASpace=NV01_NULL_OBJECT (inherit TSG), hObjectBuffer=hPbVirt, gpFifoEntries=32,
      gpFifoOffset=0, hUserdMemory[0]=hUserd, userdOffset[0]=0, hContextShare=NV01_NULL_OBJECT,
      engineType=NV2080_ENGINE_TYPE_GR0, flags=DRF_DEF(OS04,_FLAGS,_CC_SECURE,_TRUE)};
      AllocWithHandle(pRmApi,hClient,hTsg,hChannel,chClass,...) -- parented to L1 TSG;
  (8) BLACKWELL_COMPUTE_A: AllocWithHandle(pRmApi,hClient,hChannel,hCompute,0xCDC0,NULL,0).
  *phChannel=hChannel now (was 0). Pool struct extended: hPbPhys,hPbVirt,hUserd,hCompute.
  Cleanup simplified to single cleanup_base: (client-free cascades all). NV_PRINTF per step.
- Includes added: class/cl003e.h(NV01_MEMORY_SYSTEM), cl50a0.h(NV50_MEMORY_VIRTUAL),
  cla06f.h(KeplerAControlGPFifo,NVA06F_GP_ENTRY__SIZE), clcdc0.h(BLACKWELL_COMPUTE_A),
  alloc/alloc_channel.h(NV_CHANNEL_ALLOC_PARAMS,NVOS04_FLAGS_CC_SECURE),
  gpu/mem_mgr/heap.h(HEAP_OWNER_RM_CLIENT_GENERIC <- this one was the compile miss).
  NV_MEMORY_ALLOCATION_PARAMS + NVOS32_* + ATTR2_MEMORY_PROTECTION_UNPROTECTED in nvos.h.
- L2=created-not-scheduled (gpFifoOffset=0, no DMA map, no shared-mem). L3 deferred those.

## CRITICAL LOCK CORRECTION (prior "confirmed" research was WRONG)
- rmapi.c L114-118 _rmapiInitInterface flags: RMAPI_GPU_LOCK_INTERNAL has BOTH
  bApiLockInternal=TRUE AND bGpuLockInternal=TRUE => caller MUST hold BOTH API+GPU
  locks; rmapi acquires NEITHER. RMAPI_API_LOCK_INTERNAL has bGpuLockInternal=FALSE
  => RmAlloc acquires the GPU lock itself per-call (caller holds only API lock).
- So GPU_LOCK_INTERNAL does NOT "acquire the GPU lock per call". L1 originally held
  ONLY the API lock + used GPU_LOCK_INTERNAL = WRONG (would run allocs w/o GPU lock).
- FIX applied to BOTH alloc+free: mirror the in-file CE allocator
  rm_sev_gpu_alloc_cc_channel -> acquire API lock THEN rmGpuLocksAcquire(
  GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_OSAPI) (bGpuLockTaken flag), use
  GPU_LOCK_INTERNAL throughout, release GPU lock in done: before rmapiLockRelease().
  CE allocator proves both-locks works for sysmem+vaspace+channel allocs in this driver.
- (golden channel kgraphics_tu102/kernel_graphics releases GPU lock for VidHeapControl
  per Bug 1735851 contention w/ UVM/CUDA -- not a correctness req at manager provision
  time; both-locks is the proven-in-THIS-file pattern.)
- NV_CHANNEL_ALLOC_PARAMS.hObjectBuffer comment says "no longer used" BUT golden
  channel still sets it = hPbVirt; harmless, we set it too.

## Grounding (verified in-tree, sev-changes/nvidia-driver)
- BASE TREE helper EXISTS: rmapi_utils.c rmapiutilAllocClientAndDeviceHandles(
  pRmApi,pGpu,&hClient,&hDevice,&hSubDevice) -> NV01_ROOT + NV01_DEVICE_0
  (NV0080_ALLOC_PARAMETERS{deviceId=gpuGetDeviceInstance,hClientShare=hClient}) +
  NV20_SUBDEVICE_0 (NV2080_ALLOC_PARAMETERS{subDeviceId=gpumgrGetSubDeviceInstanceFromGpu}).
  Teardown: rmapiutilFreeClientAndDeviceHandles. Uses pRmApi->AllocWithHandle +
  serverutilGenResourceHandle. NO manual GPU lock (interface locks per call).
- AUTHORITATIVE full channel ref: nv_gpu_ops.c channelAllocate (~L5737-6130) +
  nvGpuOpsChannelAllocate (L6266) + TSG alloc (L6409-6415 KEPLER_CHANNEL_GROUP_A,
  tsgParams.engineType=gpuGetNv2080EngineType(...)) + VASpace (L2511-2570
  FERMI_VASPACE_A). GPFIFO params (L5955-6012): gpFifoOffset=GPU VA of pushbuffer,
  hUserdMemory[subdev]=hUserdPhysHandle, userdOffset=0, engineType, hObjectError,
  hObjectBuffer, gpFifoEntries, flags CC_SECURE via FLD_SET_DRF(OS04,_FLAGS,
  _CC_SECURE,_TRUE) when gpuIsCCorApmFeatureEnabled. internalFlags _UVM_OWNED (skip
  for us). CRITICAL: nvGpuOps channel alloc is ENTANGLED with UVM-internal VA
  machinery (nvGpuOpsGpuMalloc, getHandleForVirtualAddr, CliGetDmaMappingInfo,
  gpuAllocInfo) -> NOT copy-pasteable; must reimplement VA alloc/map = LARGE.
  UVM only does CE/SEC2 (asserts), NOT GR -> adapt engineType to GR0.
- USERD for CC (HCC): unprotected sysmem ok (gpPutLoc SYS -> bUnprotected). Back
  USERD+pushbuffer with shared ivshmem via proven pattern in rm_sev_gpu_ce_submit
  (osapi.c L2135): memdescCreate(ADDR_SYSMEM, MEMDESC_FLAGS_ALLOC_IN_UNPROTECTED_MEMORY)
  + memdescDescribe(phys) -> wrap as NV01_MEMORY_SYSTEM_OS_DESCRIPTOR Memory handle
  -> pass as hUserdMemory. GPFIFO needs GPU VA: pRmApi->Map(memory->VASpace).
- Lock model for allocator: mirror rmapiutil (pRmApi=rmapiGetInterface(
  RMAPI_GPU_LOCK_INTERNAL); AllocWithHandle, NO manual rmGpuLocksAcquire). The CE
  allocator (rm_sev_gpu_alloc_cc_channel osapi.c L1947) holds locks ONLY because
  CeUtils objCreate needs them; pRmApi path does not.
- Classes: NV01_ROOT 0x0, NV01_DEVICE_0 0x80, NV20_SUBDEVICE_0 0x2080,
  FERMI_VASPACE_A 0x90f1 (cl90f1.h), KEPLER_CHANNEL_GROUP_A 0xa06c (cla06c.h),
  BLACKWELL_CHANNEL_GPFIFO_A 0xc96f, BLACKWELL_COMPUTE_A 0xcdc0. engineType GR0 =
  NV2080_ENGINE_TYPE_GR0 (0x1). CC flag NVOS04_FLAGS_CC_SECURE bit 2:2.
  NVC56F_CTRL_CMD_GET_KMB 0xc56f010b (need to verify C96F supports GET_KMB on VM;
  ctrlc96f.h not in sev tree, only tgpu-nvdriver).
- osapi.c includes already: rmapi_utils.h, rs_utils.h, ctrlc56f.h, kernel_channel.h,
  kernel_fifo.h. NEED to add cl90f1.h, cla06c.h, clc96f.h, clcdc0.h, alloc/
  alloc_channel.h, class/cl0080.h, cl2080.h headers for the allocator.

## STAGED build plan (do NOT drop blind; VM-verify each layer)
L1 base tree: client/device/subdevice (helper) + VASpace(FERMI_VASPACE_A) +
   TSG(KEPLER_CHANNEL_GROUP_A, engineType GR0). Verify manager dmesg builds GR TSG.
L2 channel: USERD+GPFIFO memdesc over shared ivshmem -> OS_DESCRIPTOR mem handle ->
   map GPFIFO to VA -> BLACKWELL_CHANNEL_GPFIFO_A(CC_SECURE,hUserdMemory,gpFifoOffset)
   + BLACKWELL_COMPUTE_A. Schedule channel. Return hClient+channelId.
L3 export wiring: EXPORT_SYMBOL_GPL wrapper sev_gpu_rm_alloc_compute_channel in
   nv.c + protos in BOTH nv.h + --undefined in exports_link_command.txt (avoid
   orphan trap: define BEFORE adding export line).
L4 manager: compute-pool (add `kind` to sev_gpu_cc_chan or parallel pool) + bind
   chan_alloc_compute_fn (symbol_get/put) + provision + assign via existing
   sev_gpu_assign_channel + KMB via kmb_fetch_fn. Then flip enforce_channel_ownership.

## OPEN QUESTION blocking design (needs user VM test)
Run ./test.sh cuda-proof client; share client dmesg "SEV: client mmap" + manager
dmesg "SEV: replay" lines. Reveals: does CUDA's replayed compute channel issue its
own GET_KMB (0xc56f010b / C96F)? If yes the compute channel needs its own sealed
KMB; if CUDA rides plaintext control-reply that's a confidentiality hole. Decides
whether L4 KMB delivery is per-compute-channel.
