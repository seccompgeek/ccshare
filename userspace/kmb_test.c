/*
 * kmb_test.c
 *
 * Phase D4.1 sealed-KMB exchange self-test (cross-VM).
 *
 * The manager owns and assigns every GPU channel; a client only ever receives
 * the KMB for a channel the manager assigned to it. This tool drives the
 * in-kernel exchange that runs entirely below userspace:
 *
 *   Manager VM:  kmb_test manager <vm_id> <channel_id> [keyspace]
 *                  -> SEV_GPU_IOC_ASSIGN_CHANNEL  (record assignment + stage KMB)
 *                  -> SEV_GPU_IOC_SEND_KMB        (seal under comm key, post, wait ack)
 *
 *   Client VM:   kmb_test client
 *                  -> SEV_GPU_IOC_RECV_KMB        (wait, unseal, install, ack)
 *
 * Both ends print an 8-byte fingerprint of the PLAINTEXT KMB. The mechanism is
 * proven when the fingerprints match while the host only ever sees GCM
 * ciphertext in the shared BAR. The comm key must already be installed on both
 * ends (run the keybroker first).
 *
 * Prereq run order:
 *   1. host:        ./test.sh gen-certs ; sync+reload both VMs
 *   2. manager VM:  ./test.sh keybroker manager 0   (waits)
 *   3. client  VM:  ./test.sh keybroker client 0
 *   4. manager VM:  sudo ./kmb_test manager 0 100   (waits for the client)
 *   5. client  VM:  sudo ./kmb_test client
 *
 * Build:  make kmb_test   (in userspace/)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "sev_gpu_manager.h"

#define DEV_PATH "/dev/sev_gpu_manager"
#define DATA_DEV_PATH "/dev/sev_gpu_data0"   /* this VM's private data region */

static void print_fp(const char *label, const uint8_t fp[8])
{
    printf("%s%02x%02x%02x%02x%02x%02x%02x%02x\n", label,
           fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7]);
}

static int run_manager(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_assign_t as;
    sev_gpu_ioctl_send_kmb_t sk;

    if (argc < 4) {
        fprintf(stderr, "usage: kmb_test manager <vm_id> <channel_id> [keyspace]\n");
        return 2;
    }

    memset(&as, 0, sizeof(as));
    as.vm_id      = (uint8_t)strtoul(argv[2], NULL, 0);
    as.channel_id = (uint32_t)strtoul(argv[3], NULL, 0);
    as.keyspace   = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;

    printf("=== sealed-KMB self-test (manager) ===\n");
    printf("[*] assigning channel %u (keyspace %u) to VM%u...\n",
           as.channel_id, as.keyspace, as.vm_id);
    if (ioctl(fd, SEV_GPU_IOC_ASSIGN_CHANNEL, &as) < 0) {
        fprintf(stderr, "[-] ASSIGN_CHANNEL: %s\n", strerror(errno));
        return 1;
    }

    memset(&sk, 0, sizeof(sk));
    sk.vm_id      = as.vm_id;
    sk.channel_id = as.channel_id;
    sk.timeout_ms = 0; /* driver default (~2 min) */

    printf("[*] sealing + sending KMB, waiting for client to install...\n");
    if (ioctl(fd, SEV_GPU_IOC_SEND_KMB, &sk) < 0) {
        fprintf(stderr, "[-] SEND_KMB: %s\n", strerror(errno));
        if (errno == ENOKEY)
            fprintf(stderr, "    no comm key for VM%u -- run the keybroker first.\n",
                    as.vm_id);
        return 1;
    }

    print_fp("[+] client installed it. plaintext KMB fp = ", sk.fp);
    printf("[+] PASS: KMB sealed, delivered and acknowledged.\n");
    return 0;
}

static int run_client(int fd)
{
    sev_gpu_ioctl_recv_kmb_t rk;

    memset(&rk, 0, sizeof(rk));
    rk.timeout_ms = 0; /* driver default (~2 min) */

    printf("=== sealed-KMB self-test (client) ===\n");
    printf("[*] waiting for a sealed KMB from the manager...\n");
    if (ioctl(fd, SEV_GPU_IOC_RECV_KMB, &rk) < 0) {
        fprintf(stderr, "[-] RECV_KMB: %s\n", strerror(errno));
        if (errno == ENOKEY)
            fprintf(stderr, "    no comm key for this node -- run the keybroker first.\n");
        else if (errno == EBADMSG)
            fprintf(stderr, "    GCM auth failed: ciphertext or AAD tampered.\n");
        return 1;
    }

    printf("[+] received KMB for channel %u (keyspace %u)\n",
           rk.channel_id, rk.keyspace);
    print_fp("[+] plaintext KMB fp = ", rk.fp);
    printf("[+] PASS: KMB unsealed and installed.\n");
    return 0;
}

/*
 * D4.3b data-plane crypto round trip. The client must already hold an installed
 * KMB for <channel_id> (run 'kmb_test client' first). We encrypt a buffer under
 * the h2d (encrypt) bundle, then decrypt it back under the same bundle and
 * verify the plaintext is recovered -- exercising the AES-256-GCM path and the
 * CC IV derivation (gcm_iv = counter XOR ivMask) entirely in software.
 */
static int run_crypt(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_crypt_t enc, dec;
    sev_gpu_ioctl_chan_status_t cs;
    unsigned char orig[256], work[256];
    uint32_t channel_id;
    size_t i, len = sizeof(orig);

    if (argc < 3) {
        fprintf(stderr, "usage: kmb_test crypt <channel_id>\n");
        return 2;
    }
    channel_id = (uint32_t)strtoul(argv[2], NULL, 0);

    for (i = 0; i < len; i++)
        orig[i] = (unsigned char)(i * 7 + 3);
    memcpy(work, orig, len);

    printf("=== data-plane crypto self-test (client) ===\n");

    /* Observe the channel's current KMB epoch + counters. */
    memset(&cs, 0, sizeof(cs));
    cs.channel_id = channel_id;
    if (ioctl(fd, SEV_GPU_IOC_CHAN_STATUS, &cs) < 0) {
        fprintf(stderr, "[-] CHAN_STATUS: %s\n", strerror(errno));
        return 1;
    }
    if (!cs.valid) {
        fprintf(stderr, "[-] no KMB installed for channel %u -- run 'kmb_test client' first.\n",
                channel_id);
        return 1;
    }
    printf("[*] channel %u: generation %u, ctr_h2d %llu, ctr_d2h %llu\n",
           channel_id, cs.generation,
           (unsigned long long)cs.ctr_h2d, (unsigned long long)cs.ctr_d2h);

    memset(&enc, 0, sizeof(enc));
    enc.channel_id = channel_id;
    enc.flags      = 0; /* encrypt, encryptBundle */
    enc.length     = (uint32_t)len;
    enc.data       = (uint64_t)(uintptr_t)work;

    printf("[*] encrypting %zu bytes on channel %u...\n", len, channel_id);
    if (ioctl(fd, SEV_GPU_IOC_CRYPT, &enc) < 0) {
        fprintf(stderr, "[-] CRYPT(encrypt): %s\n", strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "    no KMB installed -- run 'kmb_test client' first.\n");
        else if (errno == EOVERFLOW)
            fprintf(stderr, "    IV counter exhausted -- channel needs a key rotation.\n");
        return 1;
    }
    if (memcmp(work, orig, len) == 0) {
        fprintf(stderr, "[-] ciphertext equals plaintext -- encrypt did nothing.\n");
        return 1;
    }
    printf("[+] ciphertext differs from plaintext (epoch %u, counter + tag returned).\n",
           enc.generation);

    memset(&dec, 0, sizeof(dec));
    dec.channel_id = channel_id;
    dec.flags      = SEV_GPU_CRYPT_F_DECRYPT; /* decrypt, same (encrypt) bundle */
    dec.length     = (uint32_t)len;
    dec.data       = (uint64_t)(uintptr_t)work;
    dec.generation = enc.generation;          /* pin to the epoch we encrypted under */
    memcpy(dec.iv,  enc.iv,  sizeof(dec.iv));
    memcpy(dec.tag, enc.tag, sizeof(dec.tag));

    printf("[*] decrypting back (pinned to epoch %u)...\n", dec.generation);
    if (ioctl(fd, SEV_GPU_IOC_CRYPT, &dec) < 0) {
        fprintf(stderr, "[-] CRYPT(decrypt): %s\n", strerror(errno));
        if (errno == EBADMSG)
            fprintf(stderr, "    GCM auth failed -- IV/tag/key mismatch.\n");
        else if (errno == ESTALE)
            fprintf(stderr, "    KMB rotated -- ciphertext epoch no longer installed.\n");
        return 1;
    }
    if (memcmp(work, orig, len) != 0) {
        fprintf(stderr, "[-] round trip mismatch -- decrypt != original.\n");
        return 1;
    }
    printf("[+] PASS: encrypt->decrypt round trip recovered the plaintext.\n");
    return 0;
}

/*
 * GPU stage 1: provision a pool of confidential-compute channels on the REAL
 * GPU. This is the first operation that crosses into nvidia.ko on hardware --
 * SEV_GPU_IOC_PROVISION_POOL drives the bound rm_sev_gpu_alloc_cc_channel(),
 * which builds a CeUtils object per channel. It needs nvidia.ko loaded on this
 * (manager) VM with a GPU bound; otherwise the allocator symbol is unbound and
 * the ioctl returns -ENODEV. No bounce/VRAM buffers are required yet.
 */
static int run_provision(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_provision_t pv;
    uint32_t keyspace = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
    uint32_t count    = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 1;

    memset(&pv, 0, sizeof(pv));
    pv.keyspace = keyspace;
    pv.count    = count;
    pv.flags    = 0;

    printf("=== GPU CC-channel provisioning (manager) ===\n");
    printf("[*] requesting %u channel(s) on keyspace %u from the GPU...\n",
           count, keyspace);
    if (ioctl(fd, SEV_GPU_IOC_PROVISION_POOL, &pv) < 0) {
        fprintf(stderr, "[-] PROVISION_POOL: %s\n", strerror(errno));
        if (errno == ENODEV)
            fprintf(stderr, "    no GPU provisioner bound -- is nvidia.ko loaded on "
                            "this (manager) VM with a GPU?\n");
        else if (errno == EPERM)
            fprintf(stderr, "    not the manager -- load the module with manager=1.\n");
        else if (errno == EIO)
            fprintf(stderr, "    GPU allocated 0 channels -- check dmesg for the RM status.\n");
        return 1;
    }
    printf("[+] provisioned %u/%u CC channel(s) on the real GPU.\n",
           pv.provisioned, count);
    if (pv.provisioned == 0) {
        fprintf(stderr, "[-] none provisioned.\n");
        return 1;
    }
    printf("[+] PASS: GPU CC-channel allocation path works (nvidia.ko CeUtils bridge).\n");
    return 0;
}

/*
 * GPU stage 2 (Option A): a client asks the manager to launch a CE secure-copy
 * on a channel it owns. A client cannot ring a GPU doorbell, so REQUEST_COPY
 * stages the submit parameters in its PRIVATE data region, kicks the manager
 * and blocks for the manager to drive the GPU and return the result. The
 * manager enforces ownership (the channel must be assigned to this VM) and the
 * offset/alignment framing before touching the GPU. With the module loaded
 * copy_loopback=1 the manager completes the request without a GPU so the
 * client->manager transport can be exercised on a VM with no GPU.
 */

/*
 * Map this VM's PRIVATE data region (the per-VM ivshmem-plain device). Its
 * physical pages are shared only with the manager's QEMU, so the GPU Copy Engine
 * (driven by the manager) DMAs them directly while staying SEV-isolated from
 * every other client. Returns the payload base (after the region header) and the
 * payload size; the caller munmaps *map_base over *map_len. The data fd is
 * closed here -- the mapping survives.
 */
static unsigned char *map_data_payload(void **map_base, size_t *map_len,
                                       uint64_t *payload_sz)
{
    sev_gpu_ioctl_data_info_t di;
    unsigned char *base;
    int dfd;

    dfd = open(DATA_DEV_PATH, O_RDWR | O_CLOEXEC);
    if (dfd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", DATA_DEV_PATH, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "    no per-VM data region -- relaunch the VM with its "
                            "ivshmem-plain data device attached.\n");
        return NULL;
    }

    memset(&di, 0, sizeof(di));
    if (ioctl(dfd, SEV_GPU_IOC_DATA_INFO, &di) < 0) {
        fprintf(stderr, "[-] DATA_INFO: %s\n", strerror(errno));
        close(dfd);
        return NULL;
    }

    *map_len = di.region_size;
    base = mmap(NULL, *map_len, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
    close(dfd);   /* the mapping outlives the fd */
    if (base == MAP_FAILED) {
        fprintf(stderr, "[-] mmap(data): %s\n", strerror(errno));
        return NULL;
    }
    *map_base   = base;
    *payload_sz = di.payload_size;
    return base + di.payload_off;
}

static int run_copy(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_crypt_t enc, dec;
    sev_gpu_ioctl_submit_t su;
    unsigned char *patt = NULL, *work = NULL, *payload;
    void *mbase = NULL;
    size_t mlen = 0;
    uint64_t payload_sz = 0, tag_off, iv_off, length;
    uint32_t channel_id;
    const char *mode;
    int do_rt, ret = 1;
    size_t i;

    if (argc < 3) {
        fprintf(stderr, "usage: kmb_test copy <channel_id> [length] [h2d|rt]\n");
        return 2;
    }
    channel_id = (uint32_t)strtoul(argv[2], NULL, 0);
    length     = (argc > 3) ? strtoull(argv[3], NULL, 0) : 4096;
    mode       = (argc > 4) ? argv[4] : "h2d";
    do_rt      = (strcmp(mode, "rt") == 0);
    if (!do_rt && strcmp(mode, "h2d") != 0) {
        fprintf(stderr, "[-] mode must be h2d or rt\n");
        return 2;
    }
    if (length == 0 || length > SEV_GPU_CRYPT_MAX) {
        fprintf(stderr, "[-] length must be 1..%u\n", SEV_GPU_CRYPT_MAX);
        return 2;
    }

    /* Tag + IV live in the payload after the data, 16-byte aligned. */
    tag_off = (length + (SEV_GPU_BOUNCE_ALIGN - 1)) &
              ~(uint64_t)(SEV_GPU_BOUNCE_ALIGN - 1);
    iv_off  = tag_off + SEV_GPU_BOUNCE_ALIGN;

    payload = map_data_payload(&mbase, &mlen, &payload_sz);
    if (!payload)
        return 1;
    if (iv_off + SEV_GPU_BOUNCE_ALIGN > payload_sz) {
        fprintf(stderr, "[-] data region too small for %llu-byte payload\n",
                (unsigned long long)length);
        munmap(mbase, mlen);
        return 1;
    }

    patt = malloc(length);
    work = malloc(length);
    if (!patt || !work)
        goto out;
    for (i = 0; i < length; i++)
        patt[i] = (unsigned char)(i * 7 + 3);
    memcpy(work, patt, length);

    printf("=== mediated CE secure-copy (client -> manager -> GPU) ===\n");

    /*
     * 1. Encrypt the pattern under the channel's encryptBundle (h2d) in
     *    software, so the GPU CE can authenticate + decrypt it on h2d.
     */
    memset(&enc, 0, sizeof(enc));
    enc.channel_id = channel_id;
    enc.flags      = 0;                       /* encrypt, encryptBundle */
    enc.length     = (uint32_t)length;
    enc.data       = (uint64_t)(uintptr_t)work;
    if (ioctl(fd, SEV_GPU_IOC_CRYPT, &enc) < 0) {
        fprintf(stderr, "[-] CRYPT(encrypt): %s\n", strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "    no KMB installed -- run 'kmb_test client' first.\n");
        goto out;
    }

    /* 2. Stage ciphertext + GCM tag + IV into the private data region. */
    memcpy(payload + 0,       work,    length);
    memcpy(payload + tag_off, enc.tag, sizeof(enc.tag));
    memset(payload + iv_off,  0,       SEV_GPU_BOUNCE_ALIGN);
    memcpy(payload + iv_off,  enc.iv,  sizeof(enc.iv));

    /* 3. Ask the manager to drive the GPU CE: decrypt ciphertext -> VRAM. */
    memset(&su, 0, sizeof(su));
    su.channel_id      = channel_id;
    su.flags           = SEV_GPU_SUBMIT_F_ENCRYPT;   /* host->device (h2d) */
    su.generation      = enc.generation;             /* pin to the KMB epoch */
    su.src_offset      = 0;                          /* payload ciphertext */
    su.dst_offset      = 0;                          /* VRAM offset        */
    su.length          = length;
    su.auth_tag_offset = tag_off;
    su.iv_offset       = iv_off;
    printf("[*] H2D: GPU authenticate+decrypt %llu bytes on channel %u...\n",
           (unsigned long long)length, channel_id);
    if (ioctl(fd, SEV_GPU_IOC_REQUEST_COPY, &su) < 0) {
        fprintf(stderr, "[-] REQUEST_COPY(h2d): %s\n", strerror(errno));
        if (errno == EACCES)
            fprintf(stderr, "    channel %u is not assigned to this VM -- run the KMB "
                            "exchange first.\n", channel_id);
        else if (errno == ENODEV)
            fprintf(stderr, "    no GPU CE bound and copy_loopback=0 -- load nvidia.ko + "
                            "provision, or reload the manager with loopback.\n");
        else if (errno == ETIMEDOUT)
            fprintf(stderr, "    the manager did not service the request -- is the manager "
                            "VM running?\n");
        else if (errno == ESTALE)
            fprintf(stderr, "    the channel KMB rotated since the request was pinned.\n");
        else if (errno == EPERM)
            fprintf(stderr, "    run this on a CLIENT (module loaded manager=0).\n");
        else if (errno == EIO)
            fprintf(stderr, "    GPU CE returned an error -- check the manager's dmesg "
                            "(GCM auth may have failed).\n");
        goto out;
    }
    printf("[+] H2D ok: GPU CE authenticated + decrypted the ciphertext into VRAM.\n");

    if (!do_rt) {
        printf("[+] PASS: mediated GPU secure-copy (h2d) on channel %u.\n", channel_id);
        ret = 0;
        goto out;
    }

    /* 4. Round trip: ask the GPU CE to re-encrypt VRAM back to the payload. */
    memset(&su, 0, sizeof(su));
    su.channel_id      = channel_id;
    su.flags           = 0;                          /* device->host (d2h) */
    su.generation      = enc.generation;
    su.src_offset      = 0;                          /* VRAM offset        */
    su.dst_offset      = 0;                          /* payload ciphertext */
    su.length          = length;
    su.auth_tag_offset = tag_off;
    su.iv_offset       = iv_off;
    printf("[*] D2H: GPU encrypt %llu bytes VRAM->payload on channel %u...\n",
           (unsigned long long)length, channel_id);
    if (ioctl(fd, SEV_GPU_IOC_REQUEST_COPY, &su) < 0) {
        fprintf(stderr, "[-] REQUEST_COPY(d2h): %s\n", strerror(errno));
        goto out;
    }
    printf("[+] D2H ok.\n");

    /* 5. Read back the CE output and decrypt under the decryptBundle (d2h). */
    memcpy(work, payload + 0, length);
    memset(&dec, 0, sizeof(dec));
    dec.channel_id = channel_id;
    dec.flags      = SEV_GPU_CRYPT_F_DECRYPT | SEV_GPU_CRYPT_F_USE_DECRYPT_BUNDLE;
    dec.length     = (uint32_t)length;
    dec.data       = (uint64_t)(uintptr_t)work;
    dec.generation = enc.generation;
    memcpy(dec.iv,  payload + iv_off,  sizeof(dec.iv));
    memcpy(dec.tag, payload + tag_off, sizeof(dec.tag));
    if (ioctl(fd, SEV_GPU_IOC_CRYPT, &dec) < 0) {
        fprintf(stderr, "[-] CRYPT(decrypt): %s\n", strerror(errno));
        if (errno == EBADMSG)
            fprintf(stderr, "    GCM auth failed -- CE d2h output / KMB mismatch.\n");
        goto out;
    }
    if (memcmp(work, patt, length) != 0) {
        fprintf(stderr, "[-] FAIL: recovered plaintext != staged pattern.\n");
        goto out;
    }
    printf("[+] PASS: GPU CE secure-copy round trip recovered the plaintext.\n");
    ret = 0;

out:
    free(patt);
    free(work);
    munmap(mbase, mlen);
    return ret;
}

int main(int argc, char **argv)
{
    int fd, ret;

    if (argc < 2 ||
        (strcmp(argv[1], "manager") != 0 && strcmp(argv[1], "client") != 0 &&
         strcmp(argv[1], "crypt") != 0 && strcmp(argv[1], "provision") != 0 &&
         strcmp(argv[1], "copy") != 0)) {
        fprintf(stderr,
                "usage: %s manager <vm_id> <channel_id> [keyspace]\n"
                "       %s client\n"
                "       %s crypt <channel_id>\n"
                "       %s provision [keyspace] [count]\n"
                "       %s copy <channel_id> [length] [h2d|rt]\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", DEV_PATH, strerror(errno));
        return 1;
    }

    if (strcmp(argv[1], "manager") == 0)
        ret = run_manager(fd, argc, argv);
    else if (strcmp(argv[1], "crypt") == 0)
        ret = run_crypt(fd, argc, argv);
    else if (strcmp(argv[1], "provision") == 0)
        ret = run_provision(fd, argc, argv);
    else if (strcmp(argv[1], "copy") == 0)
        ret = run_copy(fd, argc, argv);
    else
        ret = run_client(fd);

    close(fd);
    return ret;
}
