# window2.gdb -- identify WHICH check in F2 (libcuda+0xC1BA0) branches to the
# context-init teardown (call 0x260d170 at 0x2627576), and with what value.
#
# All addresses assume libcuda base 0x7ffff2564e00 (stable with
# `set disable-randomization on`). If `info sharedlibrary libcuda` shows a
# different base on your run, recompute: runtime = base + offset, where
#   REGFN-return  = +0xC2537   (eax = register_channel wrapper's return)
#   state-check   = +0xC274B   (cmpl $1, [rax+0x9410])
#   result2-check = +0xC2754
#   cleanup-entry = +0xC2767   (0x2627567 -> calls teardown 0x260d170)
#
# Usage on the CLIENT VM:
#   sudo gdb -x window2.gdb --args ./cc_compute_proof_client

set pagination off
set disable-randomization on
set breakpoint pending on
set $armed = 0

# Arm only once we're in the channel-registration phase.
# libcuda is mapped by the time this fires (register_channel is issued by
# libcuda), so it is now safe to insert the absolute-address breakpoints 2-5,
# which were created disabled below (they cannot be inserted before dlopen).
break *ioctl if ($rsi & 0xffff) == 0x1b
commands
  silent
  set $armed = 1
  enable 2 3 4 5
  printf "\n[REGCH] UVM_REGISTER_CHANNEL issued -- F2 branch traps enabled\n"
  continue
end

# --- Branch 1: REGFN return value (jne cleanup if != 0) --------------------
break *0x7ffff2627337 if $armed == 1
commands
  silent
  printf "[+0xC2537] REGFN returned eax=0x%x   (=> teardown if non-zero)\n", $eax
  continue
end

# --- Branch 2: state field [rax+0x9410] (jne cleanup if != 1) --------------
break *0x7ffff262754b if $armed == 1
commands
  silent
  printf "[+0xC274B] state[rax+0x9410]=0x%x  rax=0x%lx   (=> teardown if != 1)\n", *(unsigned int *)($rax + 0x9410), $rax
  continue
end

# --- Branch 3: result2 = -0xac(rbp) ----------------------------------------
break *0x7ffff2627554 if $armed == 1
commands
  silent
  printf "[+0xC2754] result2=0x%x\n", *(unsigned int *)($rbp - 0xac)
  continue
end

# --- Cleanup entry: STOP here (the branch just above this in the log is the culprit) ---
break *0x7ffff2627567 if $armed == 1
commands
  printf "\n===== CLEANUP ENTERED (0x2627567) -- next instr calls teardown 0x260d170 =====\n"
  printf "eax=0x%x  r14d=0x%x  rax=0x%lx\n", $eax, $r14d, $rax
  printf "The LAST [+0x...] line printed above identifies which check failed.\n"
  bt 4
end

# The four libcuda breakpoints (2-5) live in a shared object that is not mapped
# until CUDA dlopen's it, so they cannot be inserted at process start. Keep them
# disabled here; the register_channel arm above enables them once libcuda loads.
disable 2 3 4 5

run
