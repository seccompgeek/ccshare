#!/usr/bin/env bash
#
# guest_setup.sh — run INSIDE a guest VM (manager / client / normal).
#
# The host shares this source tree read-only over virtio-9p (mount_tag=sev_src).
# kbuild cannot write artifacts onto a read-only 9p mount, so this copies the
# source off the share into ~/sev-gpu, builds there, and loads the module with
# the requested role.
#
#   ./guest_setup.sh manager   # GPU-owning manager VM (manager=1)
#   ./guest_setup.sh client    # client VM            (manager=0)
#
# Env overrides: SEV_SHARE (default /mnt/sev-src), SEV_DEST (default ~/sev-gpu).

set -euo pipefail

SHARE="${SEV_SHARE:-/mnt/sev-src}"
DEST="${SEV_DEST:-$HOME/sev-gpu}"
ROLE="${1:-manager}"

case "$ROLE" in
    manager|client) ;;
    *) echo "[-] Unknown role '$ROLE' (use manager|client)" >&2; exit 1 ;;
esac

# Mount the read-only 9p share if it is not already mounted.
if ! mountpoint -q "$SHARE"; then
    # The 'sev_src' 9p tag only exists INSIDE a guest booted with the updated
    # launch script. If it's absent, we're almost certainly on the host.
    if [ ! -d /sys/bus/virtio/drivers/9pnet_virtio ] && \
       ! grep -q sev_src /sys/bus/virtio/devices/*/mount_tag 2>/dev/null; then
        echo "[-] 9p tag 'sev_src' not found." >&2
        echo "    Run this INSIDE a guest VM (manager/CVM/NormalVM), not on the host." >&2
        echo "    On the host the source is already at ~/workspace/sev-changes." >&2
        exit 1
    fi
    echo "[*] $SHARE not mounted; mounting 9p tag 'sev_src' (read-only)..."
    sudo mkdir -p "$SHARE"
    if ! sudo mount -t 9p -o trans=virtio,version=9p2000.L,ro sev_src "$SHARE"; then
        echo "[-] Mount failed. Are you inside a guest booted with the 9p share?" >&2
        exit 1
    fi
fi

echo "[*] Syncing source: $SHARE -> $DEST"
mkdir -p "$DEST"
cp -r "$SHARE/." "$DEST/"

echo "[*] Building kernel module against $(uname -r)..."
cd "$DEST"
chmod +x test.sh 2>/dev/null || true
make -C kernel
[ -d userspace ] && make -C userspace

echo "[*] Loading module (role=$ROLE)..."
./test.sh load "$ROLE"
./test.sh status || true

echo "[+] Done. Re-run after host-side edits to rebuild and reload."
