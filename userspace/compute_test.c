/*
 * compute_test.c
 *
 * L3.3 / L4 manager-allocated GR-compute-channel self-test (Arch B "Option A":
 * allocate-on-assign). The manager is the SOLE channel allocator and the SOLE
 * doorbell-ringer; a client never allocates a channel. This tool drives the new
 * SEV_GPU_IOC_ASSIGN_COMPUTE path and the generic sealed-KMB delivery + the
 * ownership-scoped work-submit lock on top of it:
 *
 *   Manager VM:  compute_test assign  <vm_id> [flags]
 *                  -> SEV_GPU_IOC_ASSIGN_COMPUTE
 *                     (carve USERD+GPFIFO from VM<vm_id>'s data region, ask
 *                      nvidia.ko to build a CC compute channel on them, fetch
 *                      its real CC_KMB) -- a manager-only smoke test.
 *
 *   Manager VM:  compute_test manager <vm_id> [flags]   (waits for the client)
 *                  -> SEV_GPU_IOC_ASSIGN_COMPUTE   (as above)
 *                  -> SEV_GPU_IOC_SEND_KMB         (seal under comm key, deliver)
 *
 *   Client VM:   compute_test client
 *                  -> SEV_GPU_IOC_RECV_KMB         (unseal, install, ack)
 *
 *   Manager VM:  compute_test submit  <vm_id> <h_client> <h_channel>
 *                  -> SEV_GPU_IOC_SUBMIT_WORK      (ownership-lock probe: only a
 *                     channel in that VM's assignment registry may be rung)
 *
 * The full cross-VM run proves: the manager builds a real compute channel
 * backed by the client's shared data region (zero-copy USERD/GPFIFO), fetches
 * its real KMB from the GPU, seals + delivers it, and -- with
 * enforce_channel_ownership=1 (now the default) -- refuses a doorbell ring on
 * any channel it did not assign to that VM. The comm key must already be
 * installed on both ends (run the keybroker first).
 *
 * Prereq run order (full cross-VM):
 *   1. host:        ./test.sh gen-certs ; sync + reload both VMs
 *   2. manager VM:  nvidia.ko loaded with the GPU bound (the allocator symbol)
 *   3. manager VM:  ./test.sh keybroker manager 0   (waits)
 *   4. client  VM:  ./test.sh keybroker client 0
 *   5. manager VM:  sudo ./compute_test manager 0   (waits for the client)
 *   6. client  VM:  sudo ./compute_test client
 *
 * Build:  make compute_test   (in userspace/)
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

#define DEV_PATH      "/dev/sev_gpu_manager"
#define DATA_DEV_PATH "/dev/sev_gpu_data0"   /* this VM's private data region */
#define WLC_ENC_PAGE  4096u                  /* enc cipher page; tag lives one page later */

static void print_fp(const char *label, const uint8_t fp[8])
{
    printf("%s%02x%02x%02x%02x%02x%02x%02x%02x\n", label,
           fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7]);
}

/*
 * Why ASSIGN_COMPUTE might fail -- map the kernel errnos back to the rung that
 * produced them so a bring-up failure is self-explanatory.
 */
static void explain_assign_errno(int err, uint8_t vm_id)
{
    switch (err) {
    case EPERM:
        fprintf(stderr, "    not the manager -- load the module with manager=1.\n");
        break;
    case ENODEV:
        fprintf(stderr, "    no compute-channel allocator bound -- is nvidia.ko "
                        "loaded on this (manager) VM with a GPU?\n");
        break;
    case ENXIO:
        fprintf(stderr, "    VM%u has no private data region -- relaunch that VM "
                        "with its ivshmem-plain data device attached.\n", vm_id);
        break;
    case ENOSPC:
        fprintf(stderr, "    VM%u has no free channel slot -- all "
                        "per-VM compute channels are assigned.\n", vm_id);
        break;
    case EIO:
        fprintf(stderr, "    the GPU rejected the channel build or GET_KMB "
                        "failed -- check the manager's dmesg for the RM status.\n");
        break;
    default:
        break;
    }
}

/*
 * Carve + build a compute channel on the GPU for VM<vm_id>, backed by that VM's
 * shared data region. Fills @ac on success so the caller can deliver its KMB.
 */
static int do_assign_compute(int fd, sev_gpu_ioctl_assign_compute_t *ac,
                             uint8_t vm_id, uint32_t flags)
{
    memset(ac, 0, sizeof(*ac));
    ac->vm_id = vm_id;
    ac->flags = flags;

    printf("[*] assigning a GR compute channel to VM%u (flags 0x%x)...\n",
           vm_id, flags);
    if (ioctl(fd, SEV_GPU_IOC_ASSIGN_COMPUTE, ac) < 0) {
        int err = errno;
        fprintf(stderr, "[-] ASSIGN_COMPUTE: %s\n", strerror(err));
        explain_assign_errno(err, vm_id);
        return -1;
    }

    printf("[+] assigned: channel_id=%u hClient=0x%x hChannel=0x%x\n",
           ac->channel_id, ac->h_client, ac->h_channel);
    printf("[+] zero-copy backing in VM%u data region: USERD off=0x%llx "
            "GPFIFO off=0x%llx PUSH off=0x%llx pushVA=0x%llx\n", vm_id,
           (unsigned long long)ac->userd_off,
            (unsigned long long)ac->gpfifo_off,
            (unsigned long long)ac->pushbuf_off,
            (unsigned long long)ac->pushbuf_gpu_va);
    return 0;
}

/* compute_test assign <vm_id> [flags] -- manager-only allocate-on-assign smoke. */
static int run_assign(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_assign_compute_t ac;
    uint8_t  vm_id;
    uint32_t flags;

    if (argc < 3) {
        fprintf(stderr, "usage: compute_test assign <vm_id> [flags]\n");
        return 2;
    }
    vm_id = (uint8_t)strtoul(argv[2], NULL, 0);
    flags = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;

    printf("=== manager-allocated compute channel (assign-only) ===\n");
    if (do_assign_compute(fd, &ac, vm_id, flags) < 0)
        return 1;

    printf("[+] PASS: manager built a real compute channel on VM%u's data "
           "region (GR channel uses a placeholder KMB).\n", vm_id);
    printf("    register/deliver placeholder with:  compute_test manager %u   (or SEND_KMB ch %u)\n",
           vm_id, ac.channel_id);
    return 0;
}

/* compute_test manager <vm_id> [flags] -- assign then seal + deliver the KMB. */
static int run_manager(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_assign_compute_t ac;
    sev_gpu_ioctl_send_kmb_t sk;
    uint8_t  vm_id;
    uint32_t flags;

    if (argc < 3) {
        fprintf(stderr, "usage: compute_test manager <vm_id> [flags]\n");
        return 2;
    }
    vm_id = (uint8_t)strtoul(argv[2], NULL, 0);
    flags = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;

    printf("=== manager-allocated compute channel (assign + deliver) ===\n");
    if (do_assign_compute(fd, &ac, vm_id, flags) < 0)
        return 1;

    memset(&sk, 0, sizeof(sk));
    sk.vm_id      = vm_id;
    sk.channel_id = ac.channel_id;
    sk.timeout_ms = 0; /* driver default (~2 min) */

    printf("[*] sealing + sending the compute channel's KMB, waiting for the "
           "client to install...\n");
    if (ioctl(fd, SEV_GPU_IOC_SEND_KMB, &sk) < 0) {
        fprintf(stderr, "[-] SEND_KMB: %s\n", strerror(errno));
        if (errno == ENOKEY)
            fprintf(stderr, "    no comm key for VM%u -- run the keybroker first.\n",
                    vm_id);
        return 1;
    }

    print_fp("[+] client installed it. plaintext KMB fp = ", sk.fp);
    printf("[+] PASS: compute channel assigned, KMB sealed, delivered and "
           "acknowledged.\n");
    return 0;
}

/* compute_test client -- wait for the manager's sealed KMB, unseal + install. */
static int run_client(int fd)
{
    sev_gpu_ioctl_recv_kmb_t rk;

    memset(&rk, 0, sizeof(rk));
    rk.timeout_ms = 0; /* driver default (~2 min) */

    printf("=== manager-allocated compute channel (client install) ===\n");
    printf("[*] waiting for a sealed compute-channel KMB from the manager...\n");
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
    printf("[+] PASS: compute-channel KMB unsealed and installed.\n");
    return 0;
}

/*
 * compute_test submit <vm_id> <h_client> <h_channel> -- ownership-lock probe.
 *
 * Manager-local doorbell ring. With enforce_channel_ownership=1 (the default),
 * the manager only rings a channel that is in VM<vm_id>'s assignment registry;
 * an unassigned (hClient, hChannel) is refused -EACCES. Pass the vm_id + handles
 * from a prior 'assign'/'manager' run to prove an assigned channel is accepted,
 * or bogus handles to prove an unassigned one is rejected.
 */
static int run_submit(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_submit_work_t sw;

    if (argc < 5) {
        fprintf(stderr, "usage: compute_test submit <vm_id> <h_client> <h_channel>\n");
        return 2;
    }

    memset(&sw, 0, sizeof(sw));
    sw.vm_id     = (uint32_t)strtoul(argv[2], NULL, 0);
    sw.h_client  = (uint32_t)strtoul(argv[3], NULL, 0);
    sw.h_channel = (uint32_t)strtoul(argv[4], NULL, 0);

    printf("=== ownership-scoped work submit (manager doorbell) ===\n");
    printf("[*] ringing doorbell for VM%u hClient=0x%x hChannel=0x%x...\n",
           sw.vm_id, sw.h_client, sw.h_channel);
    if (ioctl(fd, SEV_GPU_IOC_SUBMIT_WORK, &sw) < 0) {
        int err = errno;
        fprintf(stderr, "[-] SUBMIT_WORK: %s\n", strerror(err));
        if (err == EACCES) {
            fprintf(stderr, "    REFUSED: this channel is not in VM%u's "
                            "assignment registry (enforcement working).\n",
                            sw.vm_id);
            fprintf(stderr, "    This is the EXPECTED result for an unassigned "
                            "channel.\n");
        } else if (err == EPERM) {
            fprintf(stderr, "    not the manager -- load the module with manager=1.\n");
        } else if (err == ENODEV) {
            fprintf(stderr, "    no GPU doorbell primitive bound and "
                            "copy_loopback=0 -- load nvidia.ko or reload the "
                            "manager with loopback.\n");
        } else if (err == EINVAL) {
            fprintf(stderr, "    bad vm_id or zero handles.\n");
        } else if (err == EIO) {
            fprintf(stderr, "    GPU rejected the submit -- check the manager's dmesg.\n");
        }
        return 1;
    }

    printf("[+] doorbell rung -- channel is assigned to VM%u (or loopback).\n",
           sw.vm_id);
    printf("[+] PASS: ownership-scoped submit accepted an assigned channel.\n");
    return 0;
}

/*
 * compute_test wlc-send <vm_id> [timeout_ms] -- manager side of the Fork B D
 * flow. The per-client WLC (Work-Launch Channel) pool must already exist (it is
 * created lazily on the first compute-channel assign to VM<vm_id>). This fetches
 * that WLC's channels[0] KMB via the in-kernel GET_KMB path, seals it under the
 * client comm key, delivers it over the sealed-KMB mailbox under the reserved id
 * SEV_GPU_WLC_CHANNEL_ID and waits for the client's ack. Run this AFTER
 * 'compute_test manager <vm_id>' (which assigns the compute channel and delivers
 * its own KMB) and while the client is blocked in 'compute_test wlc ...'.
 */
static int run_wlc_send(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_send_wlc_kmb_t sk;
    uint8_t  vm_id;

    if (argc < 3) {
        fprintf(stderr, "usage: compute_test wlc-send <vm_id> [timeout_ms]\n");
        return 2;
    }

    memset(&sk, 0, sizeof(sk));
    sk.vm_id      = (uint8_t)strtoul(argv[2], NULL, 0);
    sk.timeout_ms = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
    vm_id         = sk.vm_id;

    printf("=== Fork B D: deliver the WLC KMB to VM%u ===\n", vm_id);
    printf("[*] fetching + sealing the WLC channels[0] KMB, waiting for the "
           "client to install...\n");
    if (ioctl(fd, SEV_GPU_IOC_SEND_WLC_KMB, &sk) < 0) {
        int err = errno;
        fprintf(stderr, "[-] SEND_WLC_KMB: %s\n", strerror(err));
        if (err == EPERM)
            fprintf(stderr, "    not the manager -- load the module with manager=1.\n");
        else if (err == ENOKEY)
            fprintf(stderr, "    no comm key for VM%u -- run the keybroker first.\n", vm_id);
        else if (err == ENODEV || err == ENOENT)
            fprintf(stderr, "    no WLC pool for VM%u -- assign a compute channel "
                            "to it first (compute_test manager %u).\n", vm_id, vm_id);
        else if (err == EIO)
            fprintf(stderr, "    GET_KMB on the WLC channel failed -- check the "
                            "manager's dmesg for the RM status.\n");
        return 1;
    }

    print_fp("[+] client installed the WLC KMB. plaintext KMB fp = ", sk.fp);
    printf("[+] PASS: WLC KMB fetched, sealed, delivered and acknowledged.\n");
    return 0;
}

/*
 * Map this VM's PRIVATE data region from offset 0 (the enc region offsets the
 * kernel carves are region-relative to the region base, not the payload). The
 * fd is closed here -- the mapping survives. Returns the base or NULL.
 */
static unsigned char *map_data_region(void **map_base, size_t *map_len)
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

    base = mmap(NULL, di.region_size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
    close(dfd);   /* the mapping outlives the fd */
    if (base == MAP_FAILED) {
        fprintf(stderr, "[-] mmap(data): %s\n", strerror(errno));
        return NULL;
    }
    *map_base = base;
    *map_len  = di.region_size;
    return base;
}

/*
 * compute_test wlc <chan_idx> <push_size> -- client side of the Fork B D flow.
 * The GPU-less client:
 *   D1) waits for + installs the WLC KMB (RECV_KMB, id SEV_GPU_WLC_CHANNEL_ID);
 *       any other KMB the manager delivers first (e.g. the compute channel's) is
 *       installed too and we keep waiting for the WLC one.
 *   D2) builds a plaintext compute-method stream and AES-256-GCM-encrypts it with
 *       the WLC KMB (CRYPT, id SEV_GPU_WLC_CHANNEL_ID). The WLC's CE decrypts
 *       CPU->GPU, so we key the op with the decrypt bundle
 *       (SEV_GPU_CRYPT_F_USE_DECRYPT_BUNDLE).
 *   D3) writes the ciphertext (at enc_off) + auth tag (at enc_off + one page)
 *       into the compute channel's enc region in the PRIVATE data region, then
 *       asks the manager to decrypt-and-launch entirely on the GPU
 *       (REQUEST_WLC_LAUNCH).
 * The plaintext methods and the KMB never leave the client kernel/CVM in the
 * clear; the manager CPU only ever sees ciphertext.
 */
static int run_wlc(int fd, int argc, char **argv)
{
    sev_gpu_ioctl_compute_info_t ci;
    sev_gpu_ioctl_crypt_t        cr;
    sev_gpu_ioctl_wlc_launch_t   wl;
    unsigned char *region = NULL, *plain = NULL;
    void   *map_base = NULL;
    size_t  map_len = 0;
    uint32_t chan_idx, push_size;
    int tries;
    int rc = 1;

    if (argc < 4) {
        fprintf(stderr, "usage: compute_test wlc <chan_idx> <push_size>\n");
        return 2;
    }
    chan_idx  = (uint32_t)strtoul(argv[2], NULL, 0);
    push_size = (uint32_t)strtoul(argv[3], NULL, 0);
    if (push_size == 0 || push_size > WLC_ENC_PAGE || push_size > SEV_GPU_CRYPT_MAX) {
        fprintf(stderr, "[-] push_size must be in 1..%u (one enc cipher page)\n",
                WLC_ENC_PAGE);
        return 2;
    }

    printf("=== Fork B D: client encrypt-and-launch (chan_idx=%u, %u bytes) ===\n",
           chan_idx, push_size);

    /* D1: install the WLC KMB. Loop so a compute-channel KMB delivered first
     * doesn't derail us -- RECV_KMB installs whatever id it returns. */
    for (tries = 0; tries < 4; tries++) {
        sev_gpu_ioctl_recv_kmb_t rk;

        memset(&rk, 0, sizeof(rk));
        rk.timeout_ms = 0; /* driver default (~2 min) */
        printf("[*] waiting for a sealed KMB from the manager...\n");
        if (ioctl(fd, SEV_GPU_IOC_RECV_KMB, &rk) < 0) {
            int err = errno;
            fprintf(stderr, "[-] RECV_KMB: %s\n", strerror(err));
            if (err == ENOKEY)
                fprintf(stderr, "    no comm key for this node -- run the keybroker first.\n");
            else if (err == EBADMSG)
                fprintf(stderr, "    GCM auth failed: ciphertext or AAD tampered.\n");
            return 1;
        }
        printf("[+] installed KMB for channel %u (keyspace %u)\n",
               rk.channel_id, rk.keyspace);
        print_fp("[+] plaintext KMB fp = ", rk.fp);
        if (rk.channel_id == SEV_GPU_WLC_CHANNEL_ID) {
            printf("[+] that is the WLC KMB -- ready to encrypt.\n");
            break;
        }
        printf("[*] not the WLC KMB (0x%x); waiting for the next one...\n",
               SEV_GPU_WLC_CHANNEL_ID);
    }
    if (tries >= 4) {
        fprintf(stderr, "[-] never received the WLC KMB (id 0x%x) -- did the "
                        "manager run 'compute_test wlc-send'?\n",
                        SEV_GPU_WLC_CHANNEL_ID);
        return 1;
    }

    /* Resolve the enc region offset for this compute channel (deterministic). */
    memset(&ci, 0, sizeof(ci));
    ci.chan_idx = chan_idx;
    if (ioctl(fd, SEV_GPU_IOC_GET_COMPUTE_INFO, &ci) < 0) {
        fprintf(stderr, "[-] GET_COMPUTE_INFO: %s\n", strerror(errno));
        return 1;
    }
    printf("[+] compute channel %u enc region: cipher off=0x%llx tag off=0x%llx\n",
           chan_idx, (unsigned long long)ci.enc_off,
           (unsigned long long)(ci.enc_off + WLC_ENC_PAGE));

    /* D2: build a plaintext method stream and encrypt it with the WLC KMB. */
    plain = malloc(push_size);
    if (!plain) {
        fprintf(stderr, "[-] out of memory\n");
        return 1;
    }
    /* Deterministic filler so a HW run can diff the decrypted pushbuffer. A real
     * client would emit actual GPFIFO methods here. */
    for (uint32_t i = 0; i < push_size; i++)
        plain[i] = (unsigned char)(i & 0xff);

    memset(&cr, 0, sizeof(cr));
    cr.channel_id = SEV_GPU_WLC_CHANNEL_ID;
    cr.flags      = SEV_GPU_CRYPT_F_USE_DECRYPT_BUNDLE; /* encrypt with the CPU->GPU (decrypt) bundle */
    cr.length     = push_size;
    cr.data       = (uint64_t)(uintptr_t)plain;         /* encrypted in place */
    printf("[*] AES-256-GCM-encrypting %u bytes with the WLC KMB...\n", push_size);
    if (ioctl(fd, SEV_GPU_IOC_CRYPT, &cr) < 0) {
        int err = errno;
        fprintf(stderr, "[-] CRYPT(encrypt): %s\n", strerror(err));
        if (err == ENOKEY)
            fprintf(stderr, "    WLC KMB not installed -- D1 must succeed first.\n");
        goto out;
    }
    printf("[+] ciphertext ready (KMB epoch %u); tag+IV computed.\n", cr.generation);

    /* D3: publish ciphertext + tag into the enc region, then request the launch. */
    region = map_data_region(&map_base, &map_len);
    if (!region)
        goto out;
    if ((uint64_t)ci.enc_off + WLC_ENC_PAGE + sizeof(cr.tag) > map_len) {
        fprintf(stderr, "[-] enc region past end of data region\n");
        goto out_unmap;
    }
    memcpy(region + ci.enc_off, plain, push_size);                 /* cipher page */
    memcpy(region + ci.enc_off + WLC_ENC_PAGE, cr.tag, sizeof(cr.tag)); /* tag page */
    printf("[+] wrote %u ciphertext bytes + %zu-byte tag into the enc region.\n",
           push_size, sizeof(cr.tag));

    memset(&wl, 0, sizeof(wl));
    wl.vm_id     = 0;          /* ignored: manager derives the VM from the data region */
    wl.chan_idx  = chan_idx;
    wl.push_size = push_size;
    printf("[*] requesting the manager's WLC to decrypt-and-launch on the GPU...\n");
    if (ioctl(fd, SEV_GPU_IOC_REQUEST_WLC_LAUNCH, &wl) < 0) {
        int err = errno;
        fprintf(stderr, "[-] REQUEST_WLC_LAUNCH: %s\n", strerror(err));
        if (err == ETIMEDOUT)
            fprintf(stderr, "    manager did not complete the launch in time -- "
                            "check its dmesg.\n");
        else if (err == EIO)
            fprintf(stderr, "    the WLC launch failed on the GPU -- check the "
                            "manager's dmesg for the RM status.\n");
        goto out_unmap;
    }
    printf("[+] PASS: client-encrypted compute methods launched on the GPU "
           "(the manager CPU never saw plaintext).\n");
    rc = 0;

out_unmap:
    munmap(map_base, map_len);
out:
    free(plain);
    return rc;
}

int main(int argc, char **argv)
{
    int fd, ret;

    if (argc < 2 ||
        (strcmp(argv[1], "assign")   != 0 && strcmp(argv[1], "manager")  != 0 &&
         strcmp(argv[1], "client")   != 0 && strcmp(argv[1], "submit")   != 0 &&
         strcmp(argv[1], "wlc-send") != 0 && strcmp(argv[1], "wlc")      != 0)) {
        fprintf(stderr,
                "usage: %s assign   <vm_id> [flags]\n"
                "       %s manager  <vm_id> [flags]\n"
                "       %s client\n"
                "       %s submit   <vm_id> <h_client> <h_channel>\n"
                "       %s wlc-send <vm_id> [timeout_ms]      (manager: deliver WLC KMB)\n"
                "       %s wlc      <chan_idx> <push_size>    (client: encrypt + launch)\n",
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", DEV_PATH, strerror(errno));
        return 1;
    }

    if (strcmp(argv[1], "assign") == 0)
        ret = run_assign(fd, argc, argv);
    else if (strcmp(argv[1], "manager") == 0)
        ret = run_manager(fd, argc, argv);
    else if (strcmp(argv[1], "submit") == 0)
        ret = run_submit(fd, argc, argv);
    else if (strcmp(argv[1], "wlc-send") == 0)
        ret = run_wlc_send(fd, argc, argv);
    else if (strcmp(argv[1], "wlc") == 0)
        ret = run_wlc(fd, argc, argv);
    else
        ret = run_client(fd);

    close(fd);
    return ret;
}
