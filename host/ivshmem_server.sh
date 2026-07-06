#!/usr/bin/env bash
#
# ivshmem_server.sh
#
# Setup-time broker for the SEV GPU manager cross-VM channel.
#
# ivshmem-server creates one POSIX shared-memory object (the BAR2 backing
# store) and a UNIX socket. When each QEMU connects, the server hands it the
# shmem fd + per-peer eventfds (the doorbell interrupt path) via SCM_RIGHTS,
# assigns it an IVPosition (peer id), and then goes idle. It is NOT on the
# runtime data/interrupt path -- it is purely launch-time wiring.
#
# This server backs ONLY the shared CONTROL region (header + request/grant
# rings + doorbells) -- scheduling metadata, never GPU payload. The per-VM
# DATA staging regions are separate, private, file-backed ivshmem-plain
# devices created by qemu_ivshmem_args.sh --setup (no server involved).
#
# Run this ONCE on the host before starting the manager VM and the client VMs.
#
# Requires the `ivshmem-server` binary. It ships with QEMU
# (contrib/ivshmem-server). If it is not on PATH, point IVSHMEM_SERVER at the
# one built in this workspace's qemu/ tree.

set -euo pipefail

# --- Configuration -----------------------------------------------------------
SOCK="${IVSHMEM_SOCK:-/tmp/ivshmem_socket}"     # UNIX socket QEMU connects to
SHM_NAME="${IVSHMEM_SHM:-sev_gpu_ctrl}"         # POSIX shm name (/dev/shm/<name>)
SHM_SIZE="${IVSHMEM_SIZE:-4M}"                  # CONTROL region size (metadata only)
NUM_VECTORS="${IVSHMEM_VECTORS:-4}"             # MSI-X vectors == IVSHMEM_NUM_VECTORS
SERVER_BIN="${IVSHMEM_SERVER:-}"               # explicit path override

# --- Locate the server binary ------------------------------------------------
if [[ -z "${SERVER_BIN}" ]]; then
    if command -v ivshmem-server >/dev/null 2>&1; then
        SERVER_BIN="$(command -v ivshmem-server)"
    else
        # Fall back to known QEMU build trees on this host.
        for cand in \
            "$HOME/AMDSEV/qemu/build/contrib/ivshmem-server/ivshmem-server" \
            "$HOME/workspace/qemu/build/contrib/ivshmem-server/ivshmem-server"; do
            if [[ -x "$cand" ]]; then
                SERVER_BIN="$cand"
                break
            fi
        done
    fi
fi

if [[ -z "${SERVER_BIN}" || ! -x "${SERVER_BIN}" ]]; then
    echo "[-] ivshmem-server not found on this host." >&2
    echo "    Set IVSHMEM_SERVER=/path/to/ivshmem-server, or build it from QEMU:" >&2
    echo "      cd <qemu-src> && make contrib/ivshmem-server/ivshmem-server" >&2
    exit 1
fi

# --- Clean up any stale instance ---------------------------------------------
if [[ -S "${SOCK}" ]]; then
    echo "[*] Removing stale socket ${SOCK}"
    rm -f "${SOCK}"
fi
rm -f "/dev/shm/${SHM_NAME}"

echo "[+] ivshmem-server: ${SERVER_BIN}"
echo "    socket   = ${SOCK}"
echo "    shm name = ${SHM_NAME}  (/dev/shm/${SHM_NAME})"
echo "    shm size = ${SHM_SIZE}"
echo "    vectors  = ${NUM_VECTORS}"
echo
echo "[*] Control device is added by qemu_ivshmem_args.sh; equivalently:"
echo "      -chardev socket,path=${SOCK},id=ivshmem_ctrl"
echo "      -device  ivshmem-doorbell,chardev=ivshmem_ctrl,vectors=${NUM_VECTORS}"
echo
echo "    (The first VM to connect gets IVPosition 0 == manager peer id.)"
echo

# Run in the foreground (-F) and verbose (-v) so it is easy to see peers
# connect/disconnect. Drop -F to daemonize.
exec "${SERVER_BIN}" \
    -F -v \
    -S "${SOCK}" \
    -M "${SHM_NAME}" \
    -l "${SHM_SIZE}" \
    -n "${NUM_VECTORS}"
