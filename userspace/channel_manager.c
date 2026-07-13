// manager_app.c — Path B manager: service ONE client region, event-driven.
//
// The manager binds N devices (/dev/sev_channel0..N-1), one per client. This app
// services ONE of them (pass the client id as argv[1], default 0). Run one
// instance per client node, or extend to poll all nodes with epoll.
//
// Flow (per request):
//   1. SEV_IOC_WAIT_RING  -> block until THIS client rings its doorbell (MSI
//      raised by the manager device when KVM signalled the client's ioeventfd)
//   2. read the request from BAR2
//   3. write the reply into BAR2, flip state=DONE
//   4. SEV_IOC_COMPLETE   -> fire this client's completion irqfd (its MSI)
//
// Usage: ./manager_app [client_id]

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
    int id = (argc > 1) ? atoi(argv[1]) : 0;
    char path[64];
    struct sev_info info;
    struct sev_mailbox *mb;
    int fd;

    snprintf(path, sizeof(path), "/dev/sev_channel%d", id);
    fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }

    if (ioctl(fd, SEV_IOC_GET_INFO, &info)) { perror("GET_INFO"); return 1; }
    if (info.role != SEV_ROLE_MANAGER) {
        fprintf(stderr, "%s is not a manager device (role=%u)\n", path, info.role);
        return 1;
    }
    printf("[manager] servicing client %d via %s (shmem_size=%llu)\n",
           id, path, (unsigned long long)info.shmem_size);

    mb = mmap(NULL, info.shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mb == MAP_FAILED) { perror("mmap"); return 1; }

    for (;;) {
        /* Event-driven: block until this client rings its doorbell. */
        if (ioctl(fd, SEV_IOC_WAIT_RING)) {
            if (errno == EINTR) continue;
            perror("WAIT_RING");
            return 1;
        }

        __sync_synchronize();
        if (mb->magic != SEV_SHM_MAGIC || mb->state != SEV_ST_REQ) {
            /* Spurious/duplicate kick, or nothing staged yet — ignore. */
            continue;
        }

        unsigned seq = mb->req_seq;
        printf("[manager] client %d REQ seq=%u: \"%s\"\n",
               id, seq, (char *)mb->req);

        /* Build reply. */
        char reply[SEV_MSG_MAX];
        snprintf(reply, sizeof(reply), "ACK[%u]:%s", seq, (char *)mb->req);
        size_t rlen = strlen(reply) + 1;
        if (rlen > SEV_MSG_MAX) rlen = SEV_MSG_MAX;

        /* Publish reply FIRST, then flip state to DONE (release ordering). */
        memcpy((void *)mb->reply, reply, rlen);
        mb->reply_len = (unsigned)rlen;
        mb->reply_seq = seq;
        __sync_synchronize();
        mb->state = SEV_ST_DONE;

        /* Fire this client's completion irqfd (arg ignored; instance = client). */
        unsigned zero = 0;
        if (ioctl(fd, SEV_IOC_COMPLETE, &zero)) perror("COMPLETE");

        printf("[manager] client %d replied + completed seq=%u\n", id, seq);
    }
    return 0;
}