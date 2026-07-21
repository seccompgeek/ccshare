#!/bin/bash

# SEV GPU Manager - Test & Deploy Script
# Usage: ./test.sh [load|unload|test|clean|rebuild]

set -e

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$BASEDIR/kernel"
USERSPACE_DIR="$BASEDIR/userspace"
NVIDIA_DIR="$BASEDIR/nvidia-driver"
MODULE_NAME="sev_gpu"
IVSHMEM_PCI_ID="1af4:1110"

# Where the running guest kernel's build headers live (for the nvidia.ko build).
KBUILD_DIR="${KBUILD_DIR:-/lib/modules/$(uname -r)/build}"

# Host source tree, shared into each guest read-only over virtio-9p, and the
# local writable working copy this script builds from. Override via env.
SEV_SHARE="${SEV_SHARE:-/mnt/sev-src}"
SEV_DEST="${SEV_DEST:-$BASEDIR}"

# Role selects the kernel module 'manager' param.
#   manager (default) -> manager=1 : this VM owns the GPU / schedules grants
#   client            -> manager=0 : this VM requests GPU access
role_to_param() {
    case "${1:-manager}" in
        client|0) echo "manager=0" ;;
        manager|1|"") echo "manager=1" ;;
        *) echo "[-] Unknown role '$1' (use manager|client)" >&2; exit 1 ;;
    esac
}

# Phase A + crypto post-load verification. Reads dmesg for the markers that
# confirm (a) the comm-key handshake ran, (b) the client registered its Phase A
# nvidia.ko hooks (mmap redirect + UD-window provider). GPU-independent; the
# mmap-context/UD-aperture *use* lines only appear during cuda-proof.
phase_a_dmesg_check() {
    local role="${1:-manager}"
    echo "[*] Phase A / crypto check ($role):"
    if [ "$role" = "client" ] || [ "$role" = "0" ]; then
        if dmesg | grep -q "auto-mTLS: client handshake complete, comm key installed"; then
            echo "    [+] comm key installed (ECDHE-PSK handshake OK)"
        elif dmesg | grep -q "hs_test_kick:"; then
            echo "    [~] handshake kicked; waiting -- re-check dmesg in a moment"
        else
            echo "    [ ] no comm key yet (set HS_TEST_KICK=5 on reload, or it fires on first CUDA)"
        fi
        if dmesg | grep -q "registered UD-window provider with nvidia.ko"; then
            echo "    [+] UD-window provider registered (A2)"
        else
            echo "    [ ] UD-window provider NOT registered -- is nvidia.ko (client) loaded first?"
        fi
        if dmesg | grep -q "registered mmap redirect with nvidia.ko"; then
            echo "    [+] mmap redirect registered"
        fi
        if dmesg | grep -qi "confirm mismatch"; then
            echo "    [!] HANDSHAKE CONFIRM MISMATCH -- PSK differs between VMs or MITM"
        fi
    else
        if dmesg | grep -q "auto-mTLS: mgr FINISHED ok, comm key committed"; then
            echo "    [+] comm key committed (a client completed the handshake)"
        else
            echo "    [ ] no comm key yet (expected until a client kicks the handshake)"
        fi
    fi
}

echo "=== SEV GPU Manager Test Script ==="
echo "Base dir: $BASEDIR"
echo ""

case "${1:-test}" in
    load)
        ROLE_PARAM="$(role_to_param "${2:-manager}")"
        # Optional 3rd arg "loopback": manager echoes RM-RPC requests AND
        # completes CE copy requests with no GPU (Phase D1 / Stage 2 transport
        # test). Ignored for the client role.
        if [ "${3:-}" = "loopback" ] && [ "$ROLE_PARAM" = "manager=1" ]; then
            ROLE_PARAM="$ROLE_PARAM rpc_loopback=1 copy_loopback=1"
        fi
        echo "[*] Loading kernel module ($ROLE_PARAM)..."
        cd "$KERNEL_DIR"
        if ! lspci -d "$IVSHMEM_PCI_ID" 2>/dev/null | grep -q .; then
            echo "[!] ivshmem PCI device $IVSHMEM_PCI_ID not present."
            echo "    Probe will not bind and /dev/$MODULE_NAME will not appear."
            echo "    Add the ivshmem-doorbell device to this VM's QEMU command"
            echo "    (see host/qemu_ivshmem_args.sh) and start host/ivshmem_server.sh."
        fi
        if ! lsmod | grep -q "$MODULE_NAME"; then
            sudo insmod "$MODULE_NAME.ko" $ROLE_PARAM
            echo "[+] Module loaded"
            sleep 1
            dmesg | grep sev_gpu | tail -8
        else
            echo "[!] Module already loaded -- params NOT changed."
            echo "    The running module keeps its original role/loopback setting."
            echo "    To rebuild + apply '$ROLE_PARAM', use:"
            echo "        $0 reload ${2:-manager} ${3:-}"
        fi
        ;;

    reload)
        # Unload (if loaded) + REBUILD + load, so you never run a stale .ko.
        ROLE_PARAM="$(role_to_param "${2:-manager}")"
        if [ "${3:-}" = "loopback" ] && [ "$ROLE_PARAM" = "manager=1" ]; then
            ROLE_PARAM="$ROLE_PARAM rpc_loopback=1 copy_loopback=1"
        fi
        # Phase A / crypto: on the CLIENT, optionally auto-kick the ECDHE-PSK
        # handshake N seconds after bind (no GPU/CUDA needed to trigger it).
        # Enable with HS_TEST_KICK=<seconds> (default off). Only meaningful for
        # the client role; the manager responds passively.
        if [ "$ROLE_PARAM" = "manager=0" ] && [ -n "${HS_TEST_KICK:-}" ]; then
            ROLE_PARAM="$ROLE_PARAM hs_test_kick=$HS_TEST_KICK"
        fi
        echo "[*] Reloading kernel module ($ROLE_PARAM)..."
        cd "$KERNEL_DIR"
        if lsmod | grep -q "$MODULE_NAME"; then
            echo "[*] Unloading current module..."
            sudo rmmod "$MODULE_NAME"
        fi
        echo "[*] Rebuilding module from source..."
        make clean >/dev/null
        make
        echo "[*] Inserting freshly built module..."
        sudo insmod "$MODULE_NAME.ko" $ROLE_PARAM
        echo "[+] Module (re)loaded"
        sleep 1
        dmesg | grep sev_gpu | tail -8
        phase_a_dmesg_check "${2:-manager}"
        ;;

    sync)
        # Refresh the local working copy from the read-only 9p host share.
        # kbuild cannot write artifacts onto the read-only mount, so we build
        # from this copy; re-run after every host-side edit to avoid running a
        # stale module. Follow with: $0 reload <role>
        if ! mountpoint -q "$SEV_SHARE" 2>/dev/null && [ ! -d "$SEV_SHARE" ]; then
            echo "[-] Host share '$SEV_SHARE' not found." >&2
            echo "    Mount the 9p tag first (see guest_setup.sh), or set SEV_SHARE." >&2
            exit 1
        fi
        echo "[*] Syncing source: $SEV_SHARE -> $SEV_DEST"
        mkdir -p "$SEV_DEST"
        cp -r "$SEV_SHARE/." "$SEV_DEST/"
        echo "[+] Working copy refreshed. Now rebuild + load:"
        echo "        $0 reload <manager|client>"
        ;;

    unload)
        echo "[*] Unloading kernel module..."
        if lsmod | grep -q "$MODULE_NAME"; then
            sudo rmmod "$MODULE_NAME"
            echo "[+] Module unloaded"
        else
            echo "[!] Module not loaded"
        fi
        ;;
        
    test)
        echo "[*] Testing communication framework..."
        echo ""

        # Load module if not already loaded (role from arg 2, default manager)
        if ! lsmod | grep -q "$MODULE_NAME"; then
            ROLE_PARAM="$(role_to_param "${2:-manager}")"
            echo "[*] Loading module first ($ROLE_PARAM)..."
            cd "$KERNEL_DIR"
            sudo insmod "$MODULE_NAME.ko" $ROLE_PARAM
            sleep 1
        fi

        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- ivshmem device not bound."
            echo "    Ensure the ivshmem-doorbell device is attached and the"
            echo "    host ivshmem-server is running, then retry."
            exit 1
        fi

        # Run example
        echo "[*] Running example application..."
        echo ""
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[!] No userspace/ in this bundle (manager-only). Skipping example."
            echo "    Watch the manager with: $0 logs   (or: dmesg -w | grep sev_gpu)"
            exit 0
        fi
        cd "$USERSPACE_DIR"
        ./example_vm_app
        echo ""
        echo "[+] Test complete!"
        ;;

    rpc-test)
        # Phase D1: exercise the control-plane RM-RPC transport (client side).
        # Round-trips an opaque blob client -> manager -> client through the
        # per-VM ivshmem mailbox. Pair with a manager loaded in loopback mode
        # (on the manager VM:  $0 load manager loopback) so the blob is echoed
        # back with no GPU / nvidia.ko needed.
        echo "[*] RM-RPC transport self-test (client)..."
        echo ""

        # Load module as client if nothing is loaded yet.
        if ! lsmod | grep -q "$MODULE_NAME"; then
            echo "[*] Module not loaded; loading as client (manager=0)..."
            cd "$KERNEL_DIR"
            sudo insmod "$MODULE_NAME.ko" manager=0
            sleep 1
        fi

        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- ivshmem device not bound."
            echo "    Ensure the ivshmem-doorbell control device is attached and"
            echo "    the host ivshmem-server is running, then retry."
            exit 1
        fi

        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build the rpc_test client."
            exit 1
        fi

        # Build the test client on demand.
        if [ ! -x "$USERSPACE_DIR/rpc_test" ]; then
            echo "[*] Building rpc_test..."
            ( cd "$USERSPACE_DIR" && make rpc_test )
        fi

        echo ""
        cd "$USERSPACE_DIR"
        # cmd / size are optional positional args ($2, $3); only forward set ones.
        RPC_ARGS=()
        [ -n "${2:-}" ] && RPC_ARGS+=("$2")
        [ -n "${3:-}" ] && RPC_ARGS+=("$3")
        rc=0
        sudo ./rpc_test "${RPC_ARGS[@]}" || rc=$?
        echo ""
        echo "[*] Manager-side trace: $0 logs   (look for RM-RPC REQUEST/REPLY)"
        exit $rc
        ;;

    tunnel-test)
        # Phase D2: secure-tunnel self-test. Runs entirely on the host (or any
        # VM) -- no kernel module, ivshmem, or GPU needed. Forks a manager (TLS
        # server) and a guest (TLS client) that run mutual TLS over a pair of
        # shared-memory rings and derive a shared comm key from the handshake,
        # proving the host would only ever see ciphertext on the shared region.
        echo "[*] Secure-tunnel self-test (mutual TLS over shared-memory rings)..."
        echo ""
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build tunnel_selftest."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        # Generate the mutual-TLS certs (CA + server + client) on first run.
        if [ ! -f certs/ca_cert.pem ]; then
            echo "[*] Generating certificates..."
            sh gen_certs.sh
        fi
        # Build on demand.
        if [ ! -x tunnel_selftest ]; then
            echo "[*] Building tunnel_selftest..."
            make tunnel_selftest
        fi
        echo ""
        ./tunnel_selftest
        exit $?
        ;;

    crypt-test)
        # CC data-plane crypto known-answer self-test. Runs entirely on the
        # host (or any VM) -- no kernel module, ivshmem, or GPU needed. Pins the
        # exact wire format the in-kernel CRYPT ioctl and the GPU CE share:
        # gcm_iv = counterLE ^ ivMask, AES-256-GCM, no AAD, 16-byte tag.
        echo "[*] CC crypto known-answer self-test (host, no device)..."
        echo ""
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build cc_crypt_selftest."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        if [ ! -x cc_crypt_selftest ]; then
            echo "[*] Building cc_crypt_selftest..."
            make cc_crypt_selftest
        fi
        echo ""
        ./cc_crypt_selftest
        exit $?
        ;;

    keybroker-test)
        # Phase D2: host file-backed key-broker self-test. Forks a manager and a
        # client that run mutual TLS over a tunnel slot in a shared FILE (a stand
        # -in for the ivshmem BAR at the exact same offsets), derive a shared
        # comm key from the handshake (TLS exporter), and the manager proves the
        # key never appears in the host-readable region. No kernel/ivshmem/GPU.
        echo "[*] Key-broker self-test (mutual TLS comm-key bootstrap, host file-backed)..."
        echo ""
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build keybroker."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        [ -f certs/ca_cert.pem ] || { echo "[*] Generating certificates..."; sh gen_certs.sh; }
        [ -x keybroker ] || { echo "[*] Building keybroker..."; make keybroker; }
        VM_ID="${2:-0}"
        BAR="$(mktemp)"
        echo ""
        ./keybroker --role manager --vm "$VM_ID" --file "$BAR" &
        mgr=$!
        sleep 0.3
        ./keybroker --role client --vm "$VM_ID" --file "$BAR"; crc=$?
        wait "$mgr"; mrc=$?
        rm -f "$BAR"
        echo ""
        if [ "$mrc" -eq 0 ] && [ "$crc" -eq 0 ]; then
            echo "RESULT: PASS -- comm key agreed over TLS, never present in shared region"
            exit 0
        fi
        echo "RESULT: FAIL (manager=$mrc client=$crc)"
        exit 1
        ;;

    keybroker)
        # Phase D2: run the real key broker against the ivshmem BAR (cross-VM).
        # IMPORTANT: start the MANAGER first (it initialises the rings and then
        # waits up to ~2 min for a client), THEN start the CLIENT on its VM.
        #   manager VM:  $0 keybroker manager [vm_id]
        #   client  VM:  $0 keybroker client  [vm_id]
        ROLE="${2:-}"
        VM_ID="${3:-0}"
        if [ "$ROLE" != "manager" ] && [ "$ROLE" != "client" ]; then
            echo "Usage: $0 keybroker manager|client [vm_id]"
            exit 2
        fi
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build keybroker."
            exit 1
        fi
        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- load the module first ($0 load $ROLE)."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        # Mutual TLS REQUIRES the SAME CA/certs on both VMs. They must be
        # provisioned once (on the host) and synced identically -- never
        # regenerated per-VM, or the CAs diverge and verification fails.
        if [ ! -f certs/ca_cert.pem ] || [ ! -f certs/server_cert.pem ] || [ ! -f certs/client_cert.pem ]; then
            echo "[-] Shared certs missing in $USERSPACE_DIR/certs/."
            echo "    Generate them ONCE on the host and sync to both VMs:"
            echo "      host:    ./test.sh gen-certs"
            echo "      each VM: $0 sync"
            echo "    (Do NOT run gen_certs.sh separately per VM -- the CAs would differ.)"
            exit 1
        fi
        [ -x keybroker ] || { echo "[*] Building keybroker..."; make keybroker; }
        echo "[*] Running key broker ($ROLE, vm $VM_ID) over the ivshmem BAR..."
        [ "$ROLE" = "manager" ] && echo "[*] Waiting for the client (start it now on the client VM)..."
        echo ""
        sudo ./keybroker --role "$ROLE" --vm "$VM_ID" --dev
        exit $?
        ;;

    handshake)
        # Phase D4 AUTOMATIC handshake (cross-VM). With the module loaded
        # auto_handshake=1 (the default), delivering the comm key via the
        # keybroker (SET_COMM_KEY) makes the kernel run the sealed-KMB exchange
        # by itself -- no separate 'kmb-test manager/client' step. The manager
        # auto-assigns a channel of hs_keyspace and seals its KMB; the client
        # unseals + installs it. The SET_COMM_KEY ioctl returns immediately and
        # the exchange completes asynchronously in a kernel workqueue, so this
        # just runs the keybroker, then tails the driver log for the result.
        #   manager VM (start FIRST):  $0 handshake manager [vm_id]
        #   client  VM (start SECOND): $0 handshake client  [vm_id]
        # PREREQS (manager): a CC channel of hs_keyspace must already be
        # provisioned ($0 gpu-provision <keyspace>) so the auto-assign finds one;
        # with no GPU/allocator the manager stages a placeholder channel 0 so the
        # seal/transport path still works (loopback testing).
        ROLE="${2:-}"
        VM_ID="${3:-0}"
        if [ "$ROLE" != "manager" ] && [ "$ROLE" != "client" ]; then
            echo "Usage: $0 handshake manager|client [vm_id]"
            exit 2
        fi
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build keybroker."
            exit 1
        fi
        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- load the module first ($0 load $ROLE)."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        if [ ! -f certs/ca_cert.pem ] || [ ! -f certs/server_cert.pem ] || [ ! -f certs/client_cert.pem ]; then
            echo "[-] Shared certs missing in $USERSPACE_DIR/certs/."
            echo "    Generate ONCE on the host and sync to both VMs:"
            echo "      host:    ./test.sh gen-certs"
            echo "      each VM: $0 sync"
            exit 1
        fi
        [ -x keybroker ] || { echo "[*] Building keybroker..."; make keybroker; }
        echo "[*] Auto-handshake ($ROLE, vm $VM_ID): delivering the comm key; the"
        echo "    kernel then runs the sealed-KMB exchange on its own."
        [ "$ROLE" = "manager" ] && echo "[*] Waiting for the client (start it now on the client VM)..."
        echo ""
        sudo ./keybroker --role "$ROLE" --vm "$VM_ID" --dev
        kb_rc=$?
        if [ "$kb_rc" -ne 0 ]; then
            echo "[-] keybroker failed (rc=$kb_rc) -- comm key not delivered; no handshake."
            exit "$kb_rc"
        fi
        echo ""
        echo "[*] Comm key delivered. Waiting for the in-kernel KMB exchange to settle..."
        # The worker waits for the peer; give it a moment, then show the result.
        sleep 3
        echo "[*] Driver trace (look for 'auto-handshake ... KMB delivered/installed'):"
        dmesg | grep -E 'sev_gpu:.*(auto-handshake|assigned channel|recv KMB|sent KMB)' | tail -12
        echo ""
        echo "[*] If you see 'installed channel N' (client) / 'KMB delivered' (manager),"
        echo "    the channel is ready. Continue with the data plane, e.g.:"
        echo "      $0 kmb-test crypt <N>            # client crypto round trip"
        echo "      $0 kmb-test copy  <N> 4096 rt    # client mediated CE copy"
        exit 0
        ;;

    kmb-test)
        # Phase D4.1: drive the in-kernel sealed-KMB exchange (cross-VM). The
        # manager owns + assigns the channel and seals its KMB under the comm
        # key from the keybroker; the client unseals + installs it. Run the
        # keybroker FIRST so both nodes hold the comm key.
        #   manager VM:  $0 kmb-test manager [vm_id] [channel_id]   (waits)
        #   client  VM:  $0 kmb-test client
        ROLE="${2:-}"
        VM_ID="${3:-0}"
        CHAN="${4:-100}"
        if [ "$ROLE" != "manager" ] && [ "$ROLE" != "client" ] && [ "$ROLE" != "crypt" ] && [ "$ROLE" != "copy" ]; then
            echo "Usage: $0 kmb-test manager [vm_id] [channel_id]"
            echo "       $0 kmb-test client"
            echo "       $0 kmb-test crypt [channel_id]"
            echo "       $0 kmb-test copy  [channel_id] [length]"
            exit 2
        fi
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build kmb_test."
            exit 1
        fi
        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- load the module first ($0 load $ROLE)."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        [ -x kmb_test ] || { echo "[*] Building kmb_test..."; make kmb_test; }
        if [ "$ROLE" = "manager" ]; then
            echo "[*] Sealing + sending KMB for channel $CHAN to VM$VM_ID (start the client now)..."
            echo ""
            sudo ./kmb_test manager "$VM_ID" "$CHAN"
        elif [ "$ROLE" = "crypt" ]; then
            # D4.3b: client data-plane crypto round trip on an already-installed
            # channel KMB (run 'kmb-test client' first). vm_id slot holds chan.
            CRYPT_CHAN="${3:-100}"
            echo "[*] Running data-plane crypto self-test on channel $CRYPT_CHAN..."
            echo ""
            sudo ./kmb_test crypt "$CRYPT_CHAN"
        elif [ "$ROLE" = "copy" ]; then
            # D4.3 / Stage 2 (Option A): client asks the manager to launch a CE
            # secure-copy on a channel it owns (run 'kmb-test client' first so
            # the channel is assigned). With the manager loaded copy_loopback=1
            # this exercises the client->manager transport without a GPU.
            COPY_CHAN="${3:-100}"
            COPY_LEN="${4:-4096}"
            COPY_MODE="${5:-h2d}"
            echo "[*] Requesting a mediated CE copy on channel $COPY_CHAN ($COPY_LEN bytes, $COPY_MODE)..."
            echo ""
            sudo ./kmb_test copy "$COPY_CHAN" "$COPY_LEN" "$COPY_MODE"
        else
            echo "[*] Waiting for a sealed KMB from the manager..."
            echo ""
            sudo ./kmb_test client
        fi
        exit $?
        ;;

    compute-test)
        # L3.3 / L4: drive the manager-allocated GR-compute-channel path
        # (SEV_GPU_IOC_ASSIGN_COMPUTE). The manager carves USERD+GPFIFO from the
        # target VM's data region, asks nvidia.ko to build a CC compute channel
        # on them, fetches its real KMB, then (for the full run) seals + delivers
        # it like the CE path. Run the keybroker FIRST so both nodes hold the
        # comm key, and nvidia.ko must be loaded on the manager VM with the GPU.
        #   manager VM:  $0 compute-test assign  [vm_id]            # smoke only
        #   manager VM:  $0 compute-test manager [vm_id]            # +deliver (waits)
        #   client  VM:  $0 compute-test client
        #   manager VM:  $0 compute-test submit  <vm_id> <h_client> <h_channel>
        ROLE="${2:-}"
        if [ "$ROLE" != "assign" ] && [ "$ROLE" != "manager" ] && \
           [ "$ROLE" != "client" ] && [ "$ROLE" != "submit" ]; then
            echo "Usage: $0 compute-test assign  [vm_id]"
            echo "       $0 compute-test manager [vm_id]"
            echo "       $0 compute-test client"
            echo "       $0 compute-test submit  <vm_id> <h_client> <h_channel>"
            exit 2
        fi
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build compute_test."
            exit 1
        fi
        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- load the module first ($0 load $ROLE)."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        [ -x compute_test ] || { echo "[*] Building compute_test..."; make compute_test; }
        if [ "$ROLE" = "assign" ]; then
            VM_ID="${3:-0}"
            echo "[*] Assigning a compute channel to VM$VM_ID (manager-only smoke)..."
            echo ""
            sudo ./compute_test assign "$VM_ID"
        elif [ "$ROLE" = "manager" ]; then
            VM_ID="${3:-0}"
            echo "[*] Assigning a compute channel to VM$VM_ID and delivering its KMB (start the client now)..."
            echo ""
            sudo ./compute_test manager "$VM_ID"
        elif [ "$ROLE" = "submit" ]; then
            VM_ID="${3:-}"
            HC="${4:-}"
            HCH="${5:-}"
            if [ -z "$VM_ID" ] || [ -z "$HC" ] || [ -z "$HCH" ]; then
                echo "Usage: $0 compute-test submit <vm_id> <h_client> <h_channel>"
                exit 2
            fi
            echo "[*] Probing the ownership lock for VM$VM_ID hClient=$HC hChannel=$HCH..."
            echo ""
            sudo ./compute_test submit "$VM_ID" "$HC" "$HCH"
        else
            echo "[*] Waiting for a sealed compute-channel KMB from the manager..."
            echo ""
            sudo ./compute_test client
        fi
        exit $?
        ;;

    nvidia)
        # Build / load / unload / status the (patched) NVIDIA open GPU driver in
        # the guest. The GPU lives only on the MANAGER VM; this is what resolves
        # the rm_sev_gpu_* RM symbols the manager binds via symbol_get.
        #   $0 nvidia build      - make modules SYSSRC=<running-kernel build>
        #   $0 nvidia load          - insmod nvidia.ko (+uvm), create device nodes
        #   $0 nvidia load client   - same but with NVreg_SEVClientMode=1 (no GPU)
        #   $0 nvidia reload [role] - reload sev_gpu + NVIDIA in dependency order
        #   $0 nvidia unload        - rmmod the nvidia modules
        #   $0 nvidia status        - lsmod + nvidia-smi + /dev/nvidia*
        SUB="${2:-status}"
        if [ ! -d "$NVIDIA_DIR" ]; then
            echo "[-] No nvidia-driver/ at $NVIDIA_DIR (not in this bundle?)."
            exit 1
        fi
        case "$SUB" in
            build)
                if [ ! -d "$KBUILD_DIR" ]; then
                    echo "[-] Kernel build dir '$KBUILD_DIR' not found."
                    echo "    Install the matching linux-headers, or set KBUILD_DIR=..."
                    exit 1
                fi
                echo "[*] Building nvidia.ko against $KBUILD_DIR (this is heavy)..."
                cd "$NVIDIA_DIR"
                make modules SYSSRC="$KBUILD_DIR" -j"$(nproc)"
                echo "[+] Built:"
                ls -lh kernel-open/*.ko 2>/dev/null
                ;;
            reload)
                ROLE="${3:-manager}"
                if [ "$ROLE" != "manager" ] && [ "$ROLE" != "client" ]; then
                    echo "Usage: $0 nvidia reload [manager|client]"
                    exit 2
                fi

                echo "[*] Reloading the SEV/NVIDIA stack as $ROLE..."
                "$0" unload
                "$0" nvidia unload
                "$0" nvidia build
                "$0" nvidia load "$ROLE"
                "$0" load "$ROLE"
                echo "[+] SEV/NVIDIA stack reloaded as $ROLE"
                ;;
            load)
                cd "$NVIDIA_DIR"
                if [ ! -f kernel-open/nvidia.ko ]; then
                    echo "[-] kernel-open/nvidia.ko not built -- run: $0 nvidia build"
                    exit 1
                fi
                # Detect SEV client mode: pass NVreg_SEVClientMode=1 so nvidia.ko
                # loads without a physical GPU (synthetic /dev/nvidia0 is created
                # later by sev_gpu_manager.ko via sev_gpu_client_register_gpu).
                NV_EXTRA_PARAMS=""
                if [ "${3:-}" = "client" ]; then
                    NV_EXTRA_PARAMS="NVreg_SEVClientMode=1"
                    echo "[*] SEV client mode: loading nvidia.ko with NVreg_SEVClientMode=1"
                fi
                # If the GPU is still bound to vfio-pci it cannot bind to nvidia.
                if lsmod | grep -q '^vfio_pci'; then
                    echo "[!] vfio_pci is loaded; if the GPU is bound to it, nvidia.ko"
                    echo "    will not claim the device. Unbind it from vfio-pci first."
                fi
                echo "[*] Loading nvidia modules..."
                # shellcheck disable=SC2086
                sudo insmod kernel-open/nvidia.ko $NV_EXTRA_PARAMS || { echo "[-] insmod nvidia.ko failed"; exit 1; }
                sudo insmod kernel-open/nvidia-uvm.ko 2>/dev/null || true
                # Create /dev/nvidia* nodes (nvidia-smi does this if present).
                # In SEV client mode there is no physical GPU so nvidia-smi will
                # not run; always fall through to the manual mknod path.
                if [ "${3:-}" != "client" ] && command -v nvidia-smi >/dev/null 2>&1; then
                    sudo nvidia-smi >/dev/null 2>&1 || true
                fi
                if [ ! -e /dev/nvidia0 ] || [ ! -e /dev/nvidiactl ]; then
                    echo "[*] Creating device nodes..."
                    # nvidia.ko always uses fixed major 195 (NV_MAJOR_DEVICE_NUMBER).
                    # It appears in /proc/devices as "nvidia" not "nvidia-frontend".
                    NV_MAJOR=195
                    sudo mknod -m 666 /dev/nvidia0 c "$NV_MAJOR" 0 2>/dev/null || true
                    sudo mknod -m 666 /dev/nvidiactl c "$NV_MAJOR" 255 2>/dev/null || true
                    if [ "${3:-}" = "client" ]; then
                        echo "[*] SEV client mode: /dev/nvidia0 is a placeholder."
                        echo "    It becomes usable after sev_gpu_manager.ko loads"
                        echo "    and calls sev_gpu_client_register_gpu()."
                    fi
                fi
                echo "[+] nvidia modules loaded:"
                lsmod | grep -E '^nvidia' || echo "    (none?)"
                ls -l /dev/nvidia* 2>/dev/null || echo "    no /dev/nvidia* nodes"
                ;;
            unload)
                echo "[*] Unloading nvidia modules..."
                for m in nvidia_uvm nvidia_drm nvidia_modeset nvidia_peermem nvidia; do
                    lsmod | grep -q "^$m" && sudo rmmod "$m" 2>/dev/null || true
                done
                if lsmod | grep -q '^nvidia'; then
                    echo "[-] Some NVIDIA modules are still in use:" >&2
                    lsmod | grep '^nvidia' >&2
                    echo "    Stop their users and unload sev_gpu before retrying." >&2
                    exit 1
                fi
                echo "[+] Done."
                lsmod | grep -E '^nvidia' || echo "    nvidia: not loaded"
                ;;
            status)
                echo "[*] nvidia modules:"; lsmod | grep -E '^nvidia' || echo "    not loaded"
                echo "[*] /dev/nvidia*:"; ls -l /dev/nvidia* 2>/dev/null || echo "    none"
                command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi 2>&1 | head -15
                ;;
            *) echo "Usage: $0 nvidia build|load|reload|unload|status"; exit 2 ;;
        esac
        ;;

    cuda-proof)
        # Compute-under-CC milestone check, runnable on EITHER end.
        #   manager VM (owns the GPU):  $0 cuda-proof [num_elements]
        #   client  VM (GPU-less):      $0 cuda-proof client [num_elements]
        # On the MANAGER it runs locally on the CC GPU (the original proof).
        # On the CLIENT the SAME unmodified CUDA program runs, but its RM escape
        # ioctls are transparently forwarded to the manager's GPU by the patched
        # nvidia.ko -- THIS is the real transparent-remoting test. NOTE: the
        # forwarding currently covers the CONTROL plane (FINN-serialized RM
        # controls + flat allocs); the channel/doorbell SUBMISSION plane is not
        # wired yet, so on the client CUDA forwards cuInit/control traffic and
        # then stalls at cudaMalloc / kernel launch. Watch the client dmesg for
        # the FINN traces ('NVRM: [sev-finn]').
        ROLE="manager"; NELEM=""
        case "${2:-}" in
            client)  ROLE="client";  NELEM="${3:-}" ;;
            manager) ROLE="manager"; NELEM="${3:-}" ;;
            "")      ROLE="manager"; NELEM="" ;;
            *)       ROLE="manager"; NELEM="$2" ;;
        esac
        PROOF_DIR="$BASEDIR/compute-proof"
        if [ ! -d "$PROOF_DIR" ]; then
            echo "[-] No compute-proof/ at $PROOF_DIR (not in this bundle?)."
            exit 1
        fi
        if ! command -v nvcc >/dev/null 2>&1; then
            echo "[-] nvcc not found. Install the CUDA toolkit on this VM,"
            echo "    or add it to PATH (e.g. /usr/local/cuda/bin)."
            exit 1
        fi
        if ! lsmod | grep -q '^nvidia'; then
            echo "[!] nvidia.ko not loaded -- the kernel will not run. First:"
            if [ "$ROLE" = "client" ]; then
                echo "        $0 nvidia build && $0 nvidia load client"
            else
                echo "        $0 nvidia build && $0 nvidia load"
            fi
        fi
        if [ "$ROLE" = "client" ]; then
            echo "[*] CLIENT mode: unmodified CUDA; RM escapes forwarded to the"
            echo "    manager's GPU by the patched nvidia.ko."
            if ! lsmod | grep -q "$MODULE_NAME"; then
                echo "[!] $MODULE_NAME not loaded -- escapes cannot reach the manager."
                echo "        $0 load client"
            fi
            if [ ! -e /dev/nvidiactl ]; then
                echo "[-] /dev/nvidiactl missing -- the patched nvidia.ko is not loaded."
                echo "        $0 nvidia load client   (NVreg_SEVClientMode=1, no GPU needed)"
                exit 1
            fi
            echo "[*] The MANAGER VM must already be up (it owns the GPU and sets"
            echo "    its own CC ready state); GPU readiness is not set from the client."
        else
            # MANAGER mode: original local proof on the CC GPU.
            if command -v nvidia-smi >/dev/null 2>&1; then
                echo "[*] GPU Confidential Computing state:"
                nvidia-smi conf-compute -f 2>/dev/null || \
                    echo "    (nvidia-smi conf-compute not available on this driver)"
                # In CC mode the GPU boots in a NON-READY state that blocks all
                # compute (CUDA returns "system not yet initialized") until the
                # guest explicitly opens the gate -- normally done after
                # attestation. For this bring-up proof we set it directly. Idempotent.
                grs="$(nvidia-smi conf-compute -grs 2>/dev/null)"
                echo "    GPU ready state: ${grs:-unknown}"
                if ! echo "$grs" | grep -qi 'ready state.*ON\|: 1'; then
                    echo "[*] Setting GPU ready state (nvidia-smi conf-compute -srs 1)..."
                    nvidia-smi conf-compute -srs 1 2>&1 | sed 's/^/    /' || \
                        echo "    (could not set ready state -- CUDA may fail to init)"
                fi
            fi
        fi
        echo "[*] Building compute-proof..."
        make -C "$PROOF_DIR" ${CUDA_ARCH:+ARCH=$CUDA_ARCH}
        if [ "$ROLE" = "client" ]; then
            PROOF_BIN="$PROOF_DIR/cc_compute_proof_client"
        else
            PROOF_BIN="$PROOF_DIR/cc_compute_proof"
        fi
        echo "[*] Running $PROOF_BIN${NELEM:+ ($NELEM elements)}..."
        # Pass an explicit element count only if given; otherwise let the
        # program use its own default (an empty arg would parse to 0 -> 1).
        "$PROOF_BIN" ${NELEM:+"$NELEM"}
        rc=$?
        if [ "$ROLE" = "client" ]; then
            echo ""
            echo "[*] Phase A mmap-context trace (this VM):"
            echo "    -- UD aperture set on client device (A2):"
            dmesg | grep -E 'NVRM: SEV: client UD aperture set' | tail -4 || \
                echo "       (none -- nv->ud not populated; IS_UD_OFFSET won't classify doorbell)"
            echo "    -- native mmap-context built from manager reply (A1):"
            dmesg | grep -E 'sev_gpu: MAP_MEMORY ctx |NVRM: SEV: build_mmap_context' | tail -8 || \
                echo "       (none -- MAP_MEMORY reply carried no mm_valid, or fd missing)"
            echo "[*] Legacy client-side mmap redirect trace (fallback path):"
            dmesg | grep -E 'NVRM: SEV: mmap redirect' | tail -8 || \
                echo "    (no redirect lines -- expected if A1 native path handled the mmap)"
            echo "[*] Client-side mmap REJECTIONS (this VM):"
            dmesg | grep -E 'NVRM: SEV: client mmap not mediated|NVRM: VM: invalid mmap context' | tail -8 || \
                echo "    (no rejection lines -- no unmediated CPU map was attempted)"
            echo "[*] Client-side FINN control-plane trace (this VM):"
            dmesg | grep -E 'NVRM: \[sev-finn\]' | tail -12 || \
                echo "    (no [sev-finn] lines -- did any RM control reach the FINN path?)"
            echo "[*] Manager-side doorbell trace (run on the manager VM):  $0 logs"
            if [ "$rc" -eq 0 ]; then
                echo ""
                echo "[+] cuda-proof PASSED on the CLIENT -- cross-VM compute-under-CC proven!"
            else
                echo ""
                echo "[!] cuda-proof did not complete on the client (rc=$rc)."
                echo "    Check dmesg on both VMs for mmap redirect and flush-all traces."
            fi
            exit "$rc"
        fi
        if [ "$rc" -eq 0 ]; then
            echo "[+] cuda-proof PASSED -- compute-under-CC is proven on this VM."
            echo "    Next: run it on the CLIENT ('$0 cuda-proof client') to exercise"
            echo "    the transparent RM-escape forwarding path to this GPU."
        else
            echo "[-] cuda-proof FAILED (rc=$rc). Check nvidia.ko load + CC state above."
            echo "    If it says 'system not yet initialized', the GPU ready state is"
            echo "    not set: run 'nvidia-smi conf-compute -srs 1' and retry."
        fi
        exit "$rc"
        ;;

    gpu-setup)
        # Full per-VM setup ladder for the GPU data-plane test. Run on BOTH ends.
        #   manager VM:  $0 gpu-setup manager [vm_id] [keyspace] [count]
        #     -> build kernel+userspace, build+load nvidia.ko, load manager,
        #        provision CC channel(s) on the real GPU.
        #   client  VM:  $0 gpu-setup client
        #     -> build kernel+userspace, load sev_gpu_manager as client.
        # After BOTH are set up, run the handshake (manager FIRST each step):
        #   keybroker manager|client -> kmb-test manager|client -> kmb-test crypt
        ROLE="${2:-manager}"
        if [ "$ROLE" != "manager" ] && [ "$ROLE" != "client" ]; then
            echo "Usage: $0 gpu-setup manager|client [...]"
            exit 2
        fi
        echo "[*] === GPU data-plane setup ladder: role=$ROLE ==="
        echo ""
        # 1. Build the manager module + userspace tools.
        echo "[1/4] Building sev_gpu_manager + userspace..."
        make -C "$KERNEL_DIR"
        [ -d "$USERSPACE_DIR" ] && make -C "$USERSPACE_DIR"

        if [ "$ROLE" = "manager" ]; then
            # 2. Build + load the GPU driver (manager owns the GPU).
            echo ""
            echo "[2/4] Building + loading nvidia.ko..."
            "$0" nvidia build
            "$0" nvidia load
            if ! lsmod | grep -q '^nvidia'; then
                echo "[-] nvidia.ko not loaded -- cannot provision GPU channels. Fix and retry."
                exit 1
            fi
            # 3. Load the manager module (binds rm_sev_gpu_* from nvidia.ko).
            echo ""
            echo "[3/4] Loading sev_gpu_manager (manager=1)..."
            "$0" reload manager
            # 4. Provision CC channels on the real GPU.
            echo ""
            echo "[4/4] Provisioning CC channel(s) on the GPU..."
            "$0" gpu-provision "${3:-0}" "${4:-1}"
            echo ""
            echo "[+] Manager ready. Next (manager FIRST, then client):"
            echo "      $0 handshake manager ${VM_ID:-0}   # comm key + auto KMB exchange"
        else
            echo ""
            echo "[2/2] Loading sev_gpu_manager (manager=0)..."
            "$0" reload client
            echo ""
            echo "[+] Client ready. Next (after the manager step):"
            echo "      $0 handshake client 0              # comm key + auto KMB install"
            echo "      $0 kmb-test  crypt <channel>       # data-plane crypto round trip"
        fi
        ;;

    gpu-provision)
        # GPU stage 1: provision CC channels on the REAL GPU via nvidia.ko.
        # Needs the module loaded as manager AND nvidia.ko loaded with a GPU
        # bound (so rm_sev_gpu_alloc_cc_channel is resolved). No buffers needed.
        #   manager VM:  $0 gpu-provision [keyspace] [count]
        echo "[*] GPU CC-channel provisioning (manager, real GPU)..."
        echo ""
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle; cannot build kmb_test."
            exit 1
        fi
        if [ ! -e "/dev/$MODULE_NAME" ]; then
            echo "[-] /dev/$MODULE_NAME missing -- load the module first ($0 load manager)."
            exit 1
        fi
        if ! lsmod | grep -q '^nvidia'; then
            echo "[!] nvidia.ko does not appear to be loaded -- PROVISION_POOL will"
            echo "    return -ENODEV until the GPU RM is up and the CC allocator binds."
        fi
        cd "$USERSPACE_DIR"
        [ -x kmb_test ] || { echo "[*] Building kmb_test..."; make kmb_test; }
        echo ""
        sudo ./kmb_test provision "${2:-0}" "${3:-1}"
        rc=$?
        echo ""
        echo "[*] Driver trace: $0 logs   (look for 'provisioned N/M CC channel(s)')"
        exit $rc
        ;;

    gen-certs)
        # Generate the SINGLE shared CA + server + client cert set (run on the
        # HOST, then 'sync' to both VMs so they share one trust anchor).
        if [ ! -d "$USERSPACE_DIR" ]; then
            echo "[-] No userspace/ in this bundle."
            exit 1
        fi
        cd "$USERSPACE_DIR"
        sh gen_certs.sh "${2:-certs}"
        echo "[*] Now sync to each VM:  $0 sync   (on every VM)"
        exit $?
        ;;

    rebuild)
        echo "[*] Rebuilding everything..."
        
        # Rebuild kernel module
        echo "[*] Building kernel module..."
        cd "$KERNEL_DIR"
        make clean
        make
        
        # Rebuild user-space (skip if this is a manager-only bundle)
        if [ -d "$USERSPACE_DIR" ]; then
            echo "[*] Building user-space components..."
            cd "$USERSPACE_DIR"
            make clean
            make all
        else
            echo "[!] No userspace/ in this bundle (manager-only); kernel module only."
        fi
        
        echo "[+] Rebuild complete!"
        ls -lh "$KERNEL_DIR/$MODULE_NAME.ko" 2>/dev/null
        [ -d "$USERSPACE_DIR" ] && ls -lh "$USERSPACE_DIR/libsev_gpu.so" "$USERSPACE_DIR/example_vm_app" 2>/dev/null
        ;;
        
    clean)
        echo "[*] Cleaning build artifacts..."
        
        # Unload if loaded
        if lsmod | grep -q "$MODULE_NAME"; then
            echo "[*] Unloading module..."
            sudo rmmod "$MODULE_NAME"
        fi
        
        # Clean kernel module
        cd "$KERNEL_DIR"
        make clean
        
        # Clean user-space (skip if this is a manager-only bundle)
        if [ -d "$USERSPACE_DIR" ]; then
            cd "$USERSPACE_DIR"
            make clean
        fi
        
        echo "[+] Clean complete!"
        ;;
        
    logs)
        echo "[*] Kernel logs (last 20 lines):"
        dmesg | grep sev_gpu | tail -20
        ;;
        
    status)
        echo "[*] Current status:"
        echo ""
        echo "[*] ivshmem PCI device ($IVSHMEM_PCI_ID):"
        if lspci -d "$IVSHMEM_PCI_ID" 2>/dev/null | grep -q .; then
            lspci -d "$IVSHMEM_PCI_ID" -v 2>/dev/null | head -12
        else
            echo "[-] Not present (no ivshmem-doorbell attached to this VM)"
        fi
        echo ""
        if lsmod | grep -q "$MODULE_NAME"; then
            echo "[+] Module: LOADED"
            modinfo "$MODULE_NAME" 2>/dev/null | head -5
        else
            echo "[-] Module: NOT LOADED"
        fi
        echo ""
        if [ -e /dev/sev_gpu_manager ]; then
            echo "[+] Device: EXISTS"
            ls -la /dev/sev_gpu_manager
        else
            echo "[-] Device: NOT FOUND"
        fi
        echo ""
        echo "[*] Build artifacts:"
        ls -lh "$KERNEL_DIR/$MODULE_NAME.ko" "$USERSPACE_DIR/example_vm_app" 2>/dev/null || echo "Not built yet"
        ;;
        
    *)
        cat <<EOF
SEV GPU Manager - Test Script

Usage: $0 [command]

Commands:
    load [role] [loopback]
                  - Load kernel module (role: manager [default] | client).
                    Append 'loopback' to a manager load to echo RM-RPC
                    requests with no GPU (Phase D1 transport test).
    reload [role] [loopback]
                  - Unload + REBUILD + load in one step (avoids running a
                    stale .ko after editing the source).
    sync          - Refresh this working copy from the read-only 9p host
                    share ($SEV_SHARE). Run after host edits, then 'reload'.
    unload        - Unload kernel module
    test [role]   - Load module (role) and run example application
    rpc-test [cmd] [size]
                  - Client RM-RPC round-trip self-test through the ivshmem
                    mailbox (pair with a manager loaded 'loopback').
    tunnel-test   - Phase D2 secure-tunnel self-test: mutual TLS + comm-key
                    derivation over shared-memory rings. Host-only, no GPU/VM.
    crypt-test    - CC data-plane crypto known-answer self-test: AES-256-GCM +
                    gcm_iv = counterLE ^ ivMask, round-trip + negative checks.
                    Host-only, no module/ivshmem/GPU needed.
    keybroker-test [vm_id]
                  - Phase D2 key-broker self-test over a shared FILE at the
                    real BAR offsets (mutual TLS + comm-key agreement, verified
                    absent from the shared region). Host-only, no GPU/VM.
    keybroker manager|client [vm_id]
                  - Run the real key broker over the ivshmem BAR (cross-VM:
                    manager on the manager VM, client on a client VM). Start
                    the MANAGER first. Needs shared certs (see gen-certs).
                    With the module loaded auto_handshake=1 (default) this ALSO
                    triggers the in-kernel KMB exchange automatically -- see
                    'handshake' for the guided one-shot version.
    handshake manager|client [vm_id]
                  - Phase D4 AUTOMATIC handshake (cross-VM). Runs the keybroker
                    to deliver the comm key; the kernel then auto-runs the
                    sealed-KMB exchange (manager assigns + seals, client unseals
                    + installs) with NO separate kmb-test step. Start the MANAGER
                    first. Manager needs a provisioned channel of hs_keyspace
                    (gpu-provision); with no GPU it stages placeholder channel 0.
    kmb-test manager|client [vm_id] [channel_id]
                  - Phase D4.1 in-kernel sealed-KMB exchange (cross-VM). The
                    manager assigns the channel and seals its KMB under the
                    comm key; the client unseals + installs it. Matching KMB
                    fingerprints + ciphertext-only in the BAR = PASS. Run the
                    keybroker first; start the manager FIRST. MANUAL path: only
                    needed when the module is loaded auto_handshake=0.
    kmb-test crypt [channel_id]
                  - Phase D4.3b client data-plane crypto self-test. Encrypts +
                    decrypts a buffer under the installed channel KMB (run
                    'kmb-test client' first). Recovered plaintext = PASS.
    compute-test assign|manager|client|submit ...
                  - L3.3/L4 manager-allocated GR-compute-channel test
                    (SEV_GPU_IOC_ASSIGN_COMPUTE). The manager carves USERD+GPFIFO
                    from the target VM's data region, builds a CC compute channel
                    on them and fetches its real KMB. 'assign [vm]' is a
                    manager-only smoke; 'manager [vm]' also seals + delivers the
                    KMB (client runs 'compute-test client'); 'submit <vm> <hc> <hch>'
                    probes the ownership lock (unassigned channel => EACCES).
                    Needs nvidia.ko on the manager; run the keybroker first.
    gpu-provision [keyspace] [count]
                  - GPU stage 1: provision CC channel(s) on the REAL GPU via
                    nvidia.ko (rm_sev_gpu_alloc_cc_channel -> CeUtils). Manager
                    VM with nvidia.ko + a bound GPU. -ENODEV = allocator unbound.
    nvidia build|load|reload|unload|status
                  - Build/load/unload the patched NVIDIA open GPU driver in the
                    guest (manager VM only). 'build' = make modules against the
                    running kernel; 'load' = insmod nvidia(+uvm) + device nodes.
    cuda-proof [client] [num_elements]
                  - Compute-under-CC milestone check. On the MANAGER VM (no
                    'client' arg) it runs the trivial CUDA kernel locally on the
                    CC GPU. With 'client' it runs the SAME unmodified CUDA on a
                    GPU-less client VM, whose patched nvidia.ko transparently
                    forwards the RM escapes to the manager's GPU -- the real
                    remoting test. Client run currently forwards the CONTROL
                    plane (watch dmesg 'NVRM: [sev-finn]') then stalls at
                    cudaMalloc/launch until the submission plane lands (todo#7).
                    Needs nvcc + nvidia.ko. Override arch via CUDA_ARCH=sm_XX.
    gpu-setup manager|client [vm_id] [keyspace] [count]
                  - Full per-VM setup ladder. manager: build+load nvidia.ko +
                    sev_gpu_manager(1) + provision channels. client: build+load
                    sev_gpu_manager(0). Then run keybroker/kmb-test (manager 1st).
    gen-certs     - Generate the ONE shared CA + certs (run on the HOST, then
                    'sync' to every VM so they share a single trust anchor).
    rebuild       - Clean and rebuild everything
    clean         - Clean all build artifacts and unload module
    logs          - Show kernel logs
    status        - Show ivshmem PCI device + module status

Examples:
    # On the manager VM (owns the GPU):
    $0 rebuild && $0 load manager
    # On a client VM (requests the GPU):
    $0 load client && $0 test client
    $0 load && dmesg -w

    # Phase D1 RM-RPC transport test (no GPU needed):
    #   manager VM:
    $0 load manager loopback
    #   client VM:
    $0 rpc-test            # or: $0 rpc-test 0xdeadbeef 512

    # Host-only self-tests (no module/ivshmem/GPU needed):
    $0 tunnel-test
    $0 crypt-test           # CC AES-256-GCM data-plane crypto known-answer test
    $0 keybroker-test       # key broker over a shared file at real BAR offsets

    # Compute-under-CC proof (manager VM, needs nvcc + nvidia.ko):
    $0 nvidia build && $0 nvidia load
    $0 cuda-proof                   # build+run a trivial CUDA kernel, verify output

    # Transparent RM-escape forwarding test (cross-VM, needs the GPU on the manager):
    #   manager VM:  bring up the GPU + manager (it replays forwarded escapes):
    $0 gpu-setup manager 0
    #   client VM:   MUST load nvidia.ko FIRST (NVreg_SEVClientMode=1), then sev_gpu_manager:
    $0 nvidia load client && $0 load client   # order matters
    $0 cuda-proof client            # unmodified CUDA; escapes forward to the GPU
    #   then on the client confirm the mmap redirect + FINN control plane:
    dmesg | grep 'SEV: mmap redirect'
    dmesg | grep '\[sev-finn\]'

    # GPU data-plane test (cross-VM, needs a GPU on the manager VM):
    #   manager VM:
    $0 gpu-setup manager 0          # build+load nvidia.ko + manager + provision
    $0 keybroker manager 0          # then (waits for client)
    $0 kmb-test  manager 0 100
    #   client VM:
    $0 gpu-setup client             # build+load sev_gpu_manager as client
    $0 keybroker client 0
    $0 kmb-test  client
    $0 kmb-test  crypt 100

    # Phase D2 key broker over the real ivshmem BAR (cross-VM):
    #   ONE-TIME on the host:  $0 gen-certs   (then '$0 sync' on every VM)
    #   manager VM (start FIRST):
    $0 load manager && $0 keybroker manager 0
    #   client VM (start SECOND):
    $0 load client && $0 keybroker client 0

    # Phase D4 AUTOMATIC handshake (cross-VM): comm key + KMB in one step.
    # With the module loaded auto_handshake=1 (default), the keybroker delivers
    # the comm key and the kernel auto-runs the sealed-KMB exchange.
    #   manager VM (start FIRST):
    $0 handshake manager 0
    #   client VM (start SECOND):
    $0 handshake client 0
    #   then on the client, drive the data plane on the assigned channel N:
    $0 kmb-test copy 0 4096 rt     # N=0 in loopback (no GPU); pool id with a GPU

    # Phase D4.1 in-kernel sealed-KMB exchange (cross-VM, MANUAL, auto_handshake=0):
    #   manager VM (start FIRST):
    $0 kmb-test manager 0 100
    #   client VM (start SECOND):
    $0 kmb-test client

    # L3.3/L4 manager-allocated compute channel (cross-VM, needs nvidia.ko on manager):
    #   manager VM:  smoke-test the allocate-on-assign path by itself:
    $0 compute-test assign 0
    #   then the full assign + KMB delivery (run the keybroker first):
    #   manager VM (start FIRST):
    $0 keybroker manager 0 && $0 compute-test manager 0
    #   client VM (start SECOND):
    $0 keybroker client 0 && $0 compute-test client
    #   prove the submission lock (use the handles printed by 'assign'/'manager'):
    $0 compute-test submit <vm_id> <h_client> <h_channel>

Host setup (separate machine): run host/ivshmem_server.sh and add the
args from host/qemu_ivshmem_args.sh to each VM's QEMU command line.
EOF
        exit 1
        ;;
esac
