#!/bin/sh
# gen_certs.sh -- generate a private CA plus CA-signed server and client certs
# for the SEV-GPU secure tunnel mutual-TLS self-test.
#
# Outputs a flat directory (default ./certs) containing:
#   ca_cert.pem      CA self-signed certificate (trust anchor, pinned by both)
#   server_cert.pem  manager (TLS server) certificate, signed by the CA
#   server_key.pem   manager private key
#   client_cert.pem  guest VM (TLS client) certificate, signed by the CA
#   client_key.pem   guest VM private key
#
# Idempotent: if the CA already exists it is reused. Use ./gen_certs.sh -f to
# force regeneration.
set -e

OUT="${1:-certs}"
if [ "$1" = "-f" ]; then
    OUT="${2:-certs}"
    rm -rf "$OUT"
fi
mkdir -p "$OUT"

KEYBITS=4096
DAYS=3650

if [ ! -f "$OUT/ca_cert.pem" ]; then
    echo "Generating CA..."
    openssl req -x509 -nodes -days "$DAYS" -newkey "rsa:$KEYBITS" \
        -keyout "$OUT/ca_key.pem" -out "$OUT/ca_cert.pem" \
        -subj "/C=US/O=SEV-GPU/CN=sev-gpu-tunnel-ca" >/dev/null 2>&1
fi

gen_leaf() {
    name="$1"; cn="$2"
    if [ -f "$OUT/${name}_cert.pem" ]; then
        echo "Reusing $name cert"
        return
    fi
    echo "Generating $name cert..."
    openssl genrsa -out "$OUT/${name}_key.pem" "$KEYBITS" >/dev/null 2>&1
    openssl req -new -key "$OUT/${name}_key.pem" \
        -out "$OUT/${name}.csr" \
        -subj "/C=US/O=SEV-GPU/CN=$cn" >/dev/null 2>&1
    openssl x509 -req -days "$DAYS" -in "$OUT/${name}.csr" \
        -CA "$OUT/ca_cert.pem" -CAkey "$OUT/ca_key.pem" -CAcreateserial \
        -out "$OUT/${name}_cert.pem" >/dev/null 2>&1
    rm -f "$OUT/${name}.csr"
}

gen_leaf server sev-gpu-manager
gen_leaf client sev-gpu-guest

# Shared 32-byte pre-shared key (PSK) that anchors the in-kernel ECDHE-PSK
# handshake (auto_mtls) in sev_gpu_manager.ko. Unlike the mTLS certs above it is
# consumed by the kernel module directly (module param psk_path), not by the
# userspace keybroker. Both VMs must receive the SAME psk.bin.
if [ ! -f "$OUT/psk.bin" ]; then
    echo "Generating shared PSK (psk.bin)..."
    head -c 32 /dev/urandom > "$OUT/psk.bin"
else
    echo "Reusing psk.bin"
fi

# NOTE: keys are left world-readable on purpose -- in this dev setup they are
# distributed to the VMs through the host-mounted read-only 9p share, and a
# guest user must be able to read them after the cp. (A real deployment would
# provision per-VM identities via SEV-SNP attestation into encrypted guest
# memory instead of a host-readable share, and would keep keys at 0600.)
chmod 644 "$OUT"/*.pem
chmod 644 "$OUT/psk.bin"
echo "Certificates ready in $OUT/"
