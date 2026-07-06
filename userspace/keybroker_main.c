/*
 * keybroker_main.c -- CLI driver for the secure key-delivery tunnel.
 *
 * Maps a view of the ivshmem control BAR (the real /dev/sev_gpu_manager device,
 * or a plain file for the host self-test) and runs one key-delivery exchange on
 * a VM's tunnel slot, as either the manager (TLS server) or a client.
 *
 * Cross-VM use:
 *   manager VM:  keybroker --role manager --vm 0
 *   client  VM:  keybroker --role client  --vm 0
 *
 * Host self-test (two processes over a shared file, no kernel/VM):
 *   keybroker --role manager --vm 0 --file /tmp/bar &
 *   keybroker --role client  --vm 0 --file /tmp/bar
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <openssl/ssl.h>

#include "sev_keybroker.h"

#define DEV_PATH "/dev/sev_gpu_manager"

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s --role manager|client [--vm N] [--dev | --file PATH]\n"
        "          [--certs DIR]\n", p);
}

int main(int argc, char **argv)
{
    const char *role = NULL, *path = DEV_PATH, *certs_dir = "certs";
    int use_file = 0, vm = 0, i, fd, rc;
    void *bar0;
    char ca[512], cert[512], key[512];
    struct sev_kb_certs certs;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--role") && i + 1 < argc)
            role = argv[++i];
        else if (!strcmp(argv[i], "--vm") && i + 1 < argc)
            vm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--certs") && i + 1 < argc)
            certs_dir = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
            use_file = 1;
            path = argv[++i];
        } else if (!strcmp(argv[i], "--dev")) {
            use_file = 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!role || (strcmp(role, "manager") && strcmp(role, "client")) ||
        vm < 0 || vm >= SEV_GPU_MAX_VMS) {
        usage(argv[0]);
        return 2;
    }

    /* Certs follow the gen_certs.sh layout: manager presents the server cert. */
    snprintf(ca,   sizeof(ca),   "%s/ca_cert.pem", certs_dir);
    if (!strcmp(role, "manager")) {
        snprintf(cert, sizeof(cert), "%s/server_cert.pem", certs_dir);
        snprintf(key,  sizeof(key),  "%s/server_key.pem",  certs_dir);
    } else {
        snprintf(cert, sizeof(cert), "%s/client_cert.pem", certs_dir);
        snprintf(key,  sizeof(key),  "%s/client_key.pem",  certs_dir);
    }
    certs.ca = ca;
    certs.cert = cert;
    certs.key = key;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    if (use_file) {
        fd = open(path, O_RDWR | O_CREAT, 0600);
        if (fd < 0) {
            perror("open file");
            return 1;
        }
        if (ftruncate(fd, SEV_GPU_TLS_MAP_SIZE) != 0) {
            perror("ftruncate");
            close(fd);
            return 1;
        }
    } else {
        fd = open(path, O_RDWR);
        if (fd < 0) {
            perror("open " DEV_PATH);
            fprintf(stderr, "  (is sev_gpu_manager loaded and ivshmem attached?)\n");
            return 1;
        }
    }

    /* The kernel mmap maps from BAR offset 0, so map enough to reach the
     * tunnel region; bar0 then points at BAR offset 0. */
    bar0 = mmap(NULL, SEV_GPU_TLS_MAP_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, 0);
    if (bar0 == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("keybroker: role=%s vm=%d src=%s tls@0x%lx slot_stride=%lu\n",
           role, vm, use_file ? path : DEV_PATH,
           (unsigned long)SEV_GPU_TLS_REGION_OFF,
           (unsigned long)SEV_GPU_TLS_SLOT_STRIDE);

    /* On the real device, pass the open fd so the derived key is delivered via
     * SEV_GPU_IOC_SET_COMM_KEY; the host self-test (file-backed BAR) passes -1. */
    if (!strcmp(role, "manager"))
        rc = sev_kb_run_manager(bar0, vm, &certs, use_file ? -1 : fd);
    else
        rc = sev_kb_run_client(bar0, vm, &certs, use_file ? -1 : fd);

    munmap(bar0, SEV_GPU_TLS_MAP_SIZE);
    close(fd);

    printf("keybroker: %s %s\n", role, rc == 0 ? "OK" : "FAILED");
    return rc == 0 ? 0 : 1;
}
