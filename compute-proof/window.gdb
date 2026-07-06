# window.gdb -- pinpoint what libcuda reads between the CE-channel
# UVM_REGISTER_CHANNEL and the first RM_FREE that begins cudaMalloc's teardown.
#
# Background (from strace): the last productive syscall before err46 is
#   ioctl(uvm_fd, 0x1b/UVM_REGISTER_CHANNEL) = 0
# and the very next syscall is
#   ioctl(rm_fd, 0x29/RM_FREE) = 0        (teardown starts here)
# with NO syscall in between. So libcuda's decision to bail is a pure in-memory
# read of an already-mapped buffer (USERD / poll pool / notifier / KMB). This
# script freezes the process at that exact moment so we can see the values it
# saw and the code that tested them.
#
# Request encoding (x86-64):
#   UVM ioctls: _IOC(_IOC_NONE,0,nr,size) -> low16 == nr, so 0x1b == REGISTER_CHANNEL
#   RM escapes: type=0x46 (NV_IOCTL_MAGIC) -> low16 == 0x4629 == RM_FREE (nr 0x29)
#
# Usage on the CLIENT VM:
#   cd ~/sev-gpu/<...>/compute-proof     # wherever cc_compute_proof_client lives
#   sudo gdb -x window.gdb --args ./cc_compute_proof_client
# (Run the proof once normally first so the binary is built by test.sh.)

set pagination off
set disable-randomization on
set breakpoint pending on

set $seen_regch = 0
set $armed = 0

# --- UVM_REGISTER_CHANNEL (type 0x00, nr 0x1b) -----------------------------
# Break at the raw entry (*ioctl) so rsi still holds the request argument.
break *ioctl if ($rsi & 0xffff) == 0x1b
commands
  silent
  set $seen_regch = $seen_regch + 1
  set $armed = 1
  printf "\n[REGCH #%d] UVM_REGISTER_CHANNEL  fd=%d req=0x%lx argp=0x%lx\n", $seen_regch, (int)$rdi, $rsi, $rdx
  bt 8
  continue
end

# --- RM_FREE (type 0x46, nr 0x29): first one after a register_channel ------
break *ioctl if (($rsi & 0xffff) == 0x4629) && $armed == 1
commands
  printf "\n===== FIRST RM_FREE AFTER REGISTER_CHANNEL -- teardown begins =====\n"
  printf "fd=%d req=0x%lx argp=0x%lx\n", (int)$rdi, $rsi, $rdx
  # Print the reliable, high-value info FIRST so a later unmapped x/ can't
  # abort the command file before we capture it.
  printf "\n--- libcuda load base (offsets = runtime_addr - base) ---\n"
  info sharedlibrary libcuda
  printf "\n--- RM_FREE params NVOS00 {hRoot, hObjectParent, hObjectOld, status} ---\n"
  x/4xw $rdx
  printf "\n--- backtrace: highest libcuda frame that is NOT a destroy/free helper is the decider ---\n"
  bt 30
  # Buffer dumps last: GPU VAs are deterministic but their CPU mappings differ
  # per run, so a stale address may be unmapped and abort the rest -- by which
  # point everything above is already captured.
  printf "\n--- USERD / GPFIFO @ 0x200600000 (channel run-state; GPU-written on real HW) ---\n"
  x/32xw 0x200600000
  printf "\n--- last mapped buf @ 0x200c00000 ---\n"
  x/16xw 0x200c00000
  printf "\n--- CE semaphore/poll pool @ 0x200a00000 (often unmapped -- kept last) ---\n"
  x/32xw 0x200a00000
  printf "\n[STOPPED] Interactive. Use FULL runtime addresses (base above) to disassemble, e.g.\n"
  printf "  disassemble <decider_ret_addr>-0x60, <decider_ret_addr>+0x10\n"
end

run
