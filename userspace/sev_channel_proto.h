#pragma once
/*
 * sev_channel_proto.h — merged Path B protocol (SEV_* names).
 * BAR-mapped shared memory + ioeventfd doorbell + irqfd completion.
 * One manager DEVICE INSTANCE per client region; the manager binds N of them.
 */
#include <linux/types.h>

#define SEV_PCI_VENDOR_ID     0x1AF4
#define SEV_PCI_DEVICE_ID     0x10F1
#define SEV_BAR_REGS          0
#define SEV_BAR_REGS_SIZE     0x1000
#define SEV_BAR_SHMEM         2

#define SEV_REG_MAGIC         0x000
#define SEV_REG_VERSION       0x004
#define SEV_REG_STATUS        0x008
#define SEV_REG_ROLE          0x00C
#define SEV_REG_VM_ID         0x010
#define SEV_REG_SHMEM_SIZE    0x014
#define SEV_REG_DOORBELL      0x01C
#define SEV_REG_COMPLETE      0x040
#define SEV_REG_IRQ_STATUS    0x030
#define SEV_REG_IRQ_MASK      0x034

#define SEV_MAGIC             0x53564348  /* "SVCH" */
#define SEV_VERSION           1

#define SEV_STATUS_DEV_READY     (1U << 0)
#define SEV_STATUS_SHMEM_ACTIVE  (1U << 1)

#define SEV_ROLE_CLIENT       0
#define SEV_ROLE_MANAGER      1

#define SEV_IRQ_COMPLETION    (1U << 0)
#define SEV_IRQ_DOORBELL      (1U << 1)

#define SEV_SHM_MAGIC   0x4D424F58   /* "MBOX" */
#define SEV_MSG_MAX     256
#define SEV_ST_IDLE     0
#define SEV_ST_REQ      1
#define SEV_ST_DONE     2

struct sev_mailbox {
    volatile __u32 magic;
    volatile __u32 state;
    volatile __u32 vm_id;
    volatile __u32 req_seq;
    volatile __u32 reply_seq;
    volatile __u32 req_len;
    volatile __u32 reply_len;
    volatile __u32 _pad;
    __u8 req[SEV_MSG_MAX];
    __u8 reply[SEV_MSG_MAX];
} __attribute__((packed));

#include <linux/ioctl.h>
#define SEV_IOC_MAGIC   'S'
#define SEV_IOC_GET_SIZE    _IOR(SEV_IOC_MAGIC, 1, __u64)
struct sev_info { __u32 vm_id; __u32 role; __u64 shmem_size; };
#define SEV_IOC_GET_INFO    _IOR(SEV_IOC_MAGIC, 2, struct sev_info)
#define SEV_IOC_DOORBELL    _IO (SEV_IOC_MAGIC, 3)
#define SEV_IOC_WAIT_COMP   _IO (SEV_IOC_MAGIC, 4)
#define SEV_IOC_COMPLETE    _IOW(SEV_IOC_MAGIC, 5, __u32)
#define SEV_IOC_WAIT_RING   _IO (SEV_IOC_MAGIC, 6)