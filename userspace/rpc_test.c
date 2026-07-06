/*
 * rpc_test.c
 *
 * Phase D1 RM-RPC transport self-test (client side).
 *
 * Opens /dev/sev_gpu_manager on a CLIENT VM (module loaded with manager=0) and
 * issues SEV_GPU_IOC_RPC_TEST. The ioctl marshals an opaque blob through this
 * client's per-VM ivshmem mailbox to the manager and blocks for the reply.
 *
 * With the manager loaded in loopback mode (manager=1 rpc_loopback=1) the blob
 * is echoed back unchanged, so this validates the full forward path -- mailbox
 * publish, doorbell, manager replay thread, reply, copy-back -- with NO GPU and
 * no nvidia.ko loaded.
 *
 * Build:  make rpc_test        (in userspace/)
 * Run:    sudo ./rpc_test [cmd] [size]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "sev_gpu_manager.h"

#define DEV_PATH "/dev/sev_gpu_manager"

int main(int argc, char **argv)
{
    sev_gpu_ioctl_rpc_test_t t;
    uint32_t cmd  = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 0xABCD1234u;
    uint32_t size = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 256u;
    int fd, ret, i, mismatch = 0;

    if (size > sizeof(t.data))
        size = sizeof(t.data);

    printf("=== RM-RPC transport self-test ===\n");
    printf("[*] device : %s\n", DEV_PATH);
    printf("[*] cmd     : 0x%08x\n", cmd);
    printf("[*] size    : %u bytes\n\n", size);

    fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", DEV_PATH, strerror(errno));
        fprintf(stderr, "    Is the module loaded as a client (manager=0)?\n");
        return 1;
    }

    memset(&t, 0, sizeof(t));
    t.cmd  = cmd;
    t.size = size;
    /* Fill a recognizable pattern so a loopback echo is byte-verifiable. */
    for (i = 0; i < (int)size; i++)
        t.data[i] = (uint8_t)(i * 7 + 1);

    printf("[*] Issuing SEV_GPU_IOC_RPC_TEST (round-trips client -> manager -> client)...\n");
    ret = ioctl(fd, SEV_GPU_IOC_RPC_TEST, &t);
    if (ret < 0) {
        fprintf(stderr, "[-] ioctl: %s\n", strerror(errno));
        if (errno == EPERM)
            fprintf(stderr, "    EPERM: this node is the manager; run on a client (manager=0).\n");
        close(fd);
        return 1;
    }

    printf("[+] ioctl returned, manager rm_status = %d\n", t.rm_status);

    /* Verify the echoed payload (loopback manager returns it unchanged). */
    for (i = 0; i < (int)size; i++) {
        if (t.data[i] != (uint8_t)(i * 7 + 1)) {
            mismatch = 1;
            fprintf(stderr, "[-] payload mismatch at byte %d: got 0x%02x want 0x%02x\n",
                    i, t.data[i], (uint8_t)(i * 7 + 1));
            break;
        }
    }

    close(fd);

    if (t.rm_status != 0) {
        fprintf(stderr, "\n[-] manager reported non-zero status (0x%x)\n", t.rm_status);
        return 1;
    }
    if (mismatch) {
        fprintf(stderr, "\n[-] FAILED: echoed payload corrupted\n");
        return 1;
    }

    printf("\n[+] PASS: %u bytes round-tripped intact through the mailbox\n", size);
    return 0;
}
