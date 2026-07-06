#!/usr/bin/env bash
#
# qemu_ivshmem_args.sh
#
# Emit the ivshmem QEMU args for the SEV GPU manager channel, with
# HARDWARE-ENFORCED per-VM isolation of the GPU-work staging regions.
#
# Topology (see sev-changes/kernel/sev_gpu_manager.h):
#
#   * CONTROL region  -- ONE shared ivshmem-doorbell device, mapped into every
#     participating VM. Carries the header + request/grant rings + doorbells.
#     Only scheduling metadata lives here, never GPU payload. Backed by the
#     ivshmem-server (see ivshmem_server.sh).
#
#   * DATA regions     -- ONE private ivshmem-plain device PER CLIENT VM, each
#     backed by its own host file (memory-backend-file, share=on). QEMU maps a
#     given data region into exactly that client VM AND the manager VM, so a
#     client physically cannot reach another client's bytes. The manager VM
#     attaches ALL data regions (it must, to feed the GPU); a client VM attaches
#     only its own. No interrupts on the data path -- signalling is the NAPI-
#     style "kick on the control doorbell, then poll the data-region header".
#
# Usage (source it, then append "${IVSHMEM_QEMU_ARGS[@]}" to your QEMU cmd):
#
#   # one-time on the host, before launching any VM:
#   SEV_GPU_MAX_VMS=8 SEV_GPU_DATA_SIZE=64M ./qemu_ivshmem_args.sh --setup
#
#   # manager VM launch script:
#   SEV_GPU_ROLE=manager source qemu_ivshmem_args.sh
#   qemu-system-x86_64 ... "${IVSHMEM_QEMU_ARGS[@]}"
#
#   # client VM launch script (client id 0..MAX-1):
#   SEV_GPU_ROLE=client SEV_GPU_CLIENT_ID=2 source qemu_ivshmem_args.sh
#   qemu-system-x86_64 ... "${IVSHMEM_QEMU_ARGS[@]}"
#
# NOTE: every VM runs under QEMU on THIS host, so all data files live here in
#       ${SEV_GPU_DATA_DIR}. --setup creates them once; both the manager and
#       the owning client map the same host file.

# --- Configuration -----------------------------------------------------------
SOCK="${IVSHMEM_SOCK:-/tmp/ivshmem_socket}"          # control-plane socket
NUM_VECTORS="${IVSHMEM_VECTORS:-4}"                  # == IVSHMEM_NUM_VECTORS
MAX_VMS="${SEV_GPU_MAX_VMS:-8}"                      # size of the data-region pool
DATA_SIZE="${SEV_GPU_DATA_SIZE:-64M}"               # per-VM data region (total) size
DATA_DIR="${SEV_GPU_DATA_DIR:-/dev/shm}"            # where the backing files live
DATA_PREFIX="${SEV_GPU_DATA_PREFIX:-sev_gpu_data}"  # file name: <prefix>_<id>

data_file() { printf '%s/%s_%d' "${DATA_DIR}" "${DATA_PREFIX}" "$1"; }

# --- One-time host setup: create the per-VM data backing files ---------------
# Files are created zero-filled; the MANAGER kernel module writes each region's
# sev_gpu_data_header_t (magic + pool_index) when it binds the device, so the
# files need no pre-formatting here.
sev_gpu_setup_data_files() {
    local i f
    echo "[+] Creating ${MAX_VMS} data region(s) of ${DATA_SIZE} in ${DATA_DIR}"
    for (( i = 0; i < MAX_VMS; i++ )); do
        f="$(data_file "$i")"
        # truncate makes a sparse file of the exact size; memory-backend-file
        # requires the file to be at least 'size' bytes.
        truncate -s "${DATA_SIZE}" "${f}"
        echo "    data[$i] = ${f}"
    done
}

# --- Emit the QEMU args ------------------------------------------------------
# Control device first (shared, all roles), then the role-appropriate data
# device(s). Data devices are listed in pool-index order so the manager's probe
# order matches the file index.
sev_gpu_build_args() {
    local role="${SEV_GPU_ROLE:-manager}"
    local i f

    IVSHMEM_QEMU_ARGS=(
        -chardev "socket,path=${SOCK},id=ivshmem_ctrl"
        -device  "ivshmem-doorbell,chardev=ivshmem_ctrl,vectors=${NUM_VECTORS}"
    )

    case "${role}" in
    manager)
        for (( i = 0; i < MAX_VMS; i++ )); do
            f="$(data_file "$i")"
            IVSHMEM_QEMU_ARGS+=(
                -object "memory-backend-file,id=sevdata${i},mem-path=${f},size=${DATA_SIZE},share=on"
                -device "ivshmem-plain,memdev=sevdata${i}"
            )
        done
        ;;
    client)
        if [[ -z "${SEV_GPU_CLIENT_ID:-}" ]]; then
            echo "[-] SEV_GPU_ROLE=client requires SEV_GPU_CLIENT_ID" >&2
            return 1
        fi
        i="${SEV_GPU_CLIENT_ID}"
        f="$(data_file "$i")"
        IVSHMEM_QEMU_ARGS+=(
            -object "memory-backend-file,id=sevdata${i},mem-path=${f},size=${DATA_SIZE},share=on"
            -device "ivshmem-plain,memdev=sevdata${i}"
        )
        ;;
    *)
        echo "[-] Unknown SEV_GPU_ROLE='${role}' (expected manager|client)" >&2
        return 1
        ;;
    esac
}

# --- Entry point -------------------------------------------------------------
# When executed directly with --setup: create the data files (and print args).
# When sourced: build IVSHMEM_QEMU_ARGS for the requested role.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    set -euo pipefail
    case "${1:-}" in
    --setup)
        sev_gpu_setup_data_files
        ;;
    *)
        sev_gpu_build_args
        printf '%s ' "${IVSHMEM_QEMU_ARGS[@]}"
        echo
        ;;
    esac
else
    sev_gpu_build_args
fi
