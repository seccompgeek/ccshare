// client_app.c — Path B client: post a request, ring the doorbell, wait for the reply.
//
// Flow:
//   1. mmap BAR2 (this client's shared region)
//   2. write request + seq into the mailbox, flip state=REQ (release order)
//   3. SEV_IOC_DOORBELL  -> BAR0 write -> ioeventfd -> manager's QEMU wakes
//   4. SEV_IOC_WAIT_COMP -> block until the manager fires our irqfd (MSI)
//   5. read the reply, verify seq matches
//
// Usage: ./client_app [message]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "sev_channel_proto.h"

int main(int argc, char **argv)
{
    const char *msg = (argc > 1) ? argv[1] : "hello from client";
    struct sev_info info;
    struct sev_mailbox *mb;
    unsigned seq;
    size_t len;
    int fd;

    /* A client binds exactly ONE device; its node is /dev/sev_channel<vm_id>.
     * We don't know vm_id yet, so try the low ids until one opens. */
    char path[64];
    fd = -1;
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/sev_channel%d", i);
        fd = open(path, O_RDWR);
        if (fd >= 0) break;
    }
    if (fd < 0) {
        fprintf(stderr, "no /dev/sev_channelN found: %s\n", strerror(errno));
        return 1;
    }

    if (ioctl(fd, SEV_IOC_GET_INFO, &info)) { perror("GET_INFO"); return 1; }
    if (info.role != SEV_ROLE_CLIENT) {
        fprintf(stderr, "%s is not a client device (role=%u)\n", path, info.role);
        return 1;
    }
    printf("[client vm_id=%u] %s  shmem_size=%llu\n",
           info.vm_id, path, (unsigned long long)info.shmem_size);

    mb = mmap(NULL, info.shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mb == MAP_FAILED) { perror("mmap"); return 1; }

    /* Initialise the mailbox on first use. */
    if (mb->magic != SEV_SHM_MAGIC) {
        mb->magic = SEV_SHM_MAGIC;
        mb->vm_id = info.vm_id;
        mb->state = SEV_ST_IDLE;
        mb->req_seq = 0;
        mb->reply_seq = 0;
    }

    seq = mb->req_seq + 1;
    len = strlen(msg) + 1;
    if (len > SEV_MSG_MAX) len = SEV_MSG_MAX;

    /* Publish payload FIRST, then flip state to REQ (release ordering). */
    memcpy((void *)mb->req, msg, len);
    mb->req_len = (unsigned)len;
    mb->req_seq = seq;
    __sync_synchronize();
    mb->state = SEV_ST_REQ;

    printf("[client vm_id=%u] posting seq=%u: \"%s\"\n", info.vm_id, seq, msg);

    /* Kick: BAR0 DOORBELL write -> KVM ioeventfd -> manager wakes. */
    if (ioctl(fd, SEV_IOC_DOORBELL)) { perror("DOORBELL"); return 1; }

    /* Block until the manager fires our completion irqfd. */
    if (ioctl(fd, SEV_IOC_WAIT_COMP)) { perror("WAIT_COMP"); return 1; }

    __sync_synchronize();
    if (mb->state == SEV_ST_DONE && mb->reply_seq == seq) {
        printf("[client vm_id=%u] reply (seq=%u): \"%s\"\n",
               info.vm_id, mb->reply_seq, (char *)mb->reply);
        printf("[client vm_id=%u] ROUND-TRIP OK\n", info.vm_id);
        return 0;
    }
    fprintf(stderr, "[client vm_id=%u] unexpected: state=%u reply_seq=%u (want %u)\n",
            info.vm_id, mb->state, mb->reply_seq, seq);
    return 2;
}