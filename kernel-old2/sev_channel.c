// SPDX-License-Identifier: GPL-2.0
/*
 * sev_channel.c — Path B channel kernel module.
 *
 * Binds the sev-channel PCI device (1af4:10f1). ONE .ko for both roles; role is
 * read from SEV_REG_ROLE at probe.
 *
 * Multi-instance by design:
 *   - CLIENT  VM binds exactly ONE device (its own region)  -> /dev/sev_channel0
 *   - MANAGER VM binds N devices (one per client region)    -> /dev/sev_channelN
 *
 * Each manager device is a SEPARATE PCI device with its OWN MSI vector, so the
 * ISR identifies which client rang purely from dev_id (the per-instance struct)
 * — no shared "which client" field, no multiplexing. This mirrors how
 * sev_gpu_manager.c keeps data_devs[] per client.
 *
 * Char dev per instance:
 *   /dev/sev_channel<vm_id>   (minor = vm_id)
 * so the manager's userspace opens the node for the client it wants to service.
 *
 * ioctls:
 *   SEV_IOC_GET_INFO / GET_SIZE      (both roles)
 *   SEV_IOC_DOORBELL   (client)  ring BAR0 DOORBELL -> ioeventfd -> manager
 *   SEV_IOC_WAIT_COMP  (client)  block until completion MSI
 *   SEV_IOC_COMPLETE   (manager) write BAR0 COMPLETE(vm_id) -> client irqfd
 *   SEV_IOC_WAIT_RING  (manager) block until THIS client's doorbell MSI
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include "sev_channel_proto.h"

#define DRV "sev_channel"

/* One char-dev minor per possible client region (vm_id in [0, SEV_MAX_MINORS)). */
#define SEV_MAX_MINORS  16

/* Manager-only ioctl: block until this client's doorbell rings. Defined here so
 * the header stays shared/minimal; keep the number distinct from the header's. */
#define SEV_IOC_WAIT_RING  _IO(SEV_IOC_MAGIC, 6)

struct sev_dev {
    struct pci_dev *pdev;
    void __iomem   *regs;         /* BAR0 */
    phys_addr_t     shmem_phys;   /* BAR2 */
    size_t          shmem_size;
    int             irq;
    bool            irq_requested;
    u32             role;
    u32             vm_id;        /* this instance's client id (== minor)     */

    /* client: completion wait; manager: doorbell(ring) wait */
    wait_queue_head_t comp_wq;
    atomic_t          comp_pending;
    wait_queue_head_t ring_wq;
    atomic_t          ring_pending;

    dev_t           devt;
    struct cdev     cdev;
    struct device  *dev;
    bool            cdev_added;
};

/* Per-vm_id instance table (manager holds up to SEV_MAX_MINORS; client holds 1). */
static struct sev_dev *g_devs[SEV_MAX_MINORS];
static DEFINE_MUTEX(g_devs_lock);

static struct class *sev_class;
static dev_t sev_devt_base;        /* base of a contiguous minor range         */

/* ------------------------------------------------------------------ */
/* Interrupt — role-aware.                                             */
/* ------------------------------------------------------------------ */
static irqreturn_t sev_irq(int irq, void *data)
{
    struct sev_dev *d = data;

    /*
     * The interrupt is delivered by an irqfd (KVM injects the MSI directly), so
     * it does NOT pass through the QEMU device model and there is NO reliable
     * IRQ_STATUS write behind it. The MSI on THIS device's single vector IS the
     * signal: client => completion, manager => a client rang. Do NOT gate on
     * IRQ_STATUS (reading it would see 0 and drop the event).
     *
     * We still write IRQ_STATUS to ack, which is harmless and clears the
     * device's shadow bit if the msi_notify fallback path happened to set it.
     */
    if (d->role == SEV_ROLE_MANAGER) {
        atomic_set(&d->ring_pending, 1);
        wake_up_interruptible(&d->ring_wq);
        iowrite32(SEV_IRQ_DOORBELL, d->regs + SEV_REG_IRQ_STATUS);
    } else {
        atomic_set(&d->comp_pending, 1);
        wake_up_interruptible(&d->comp_wq);
        iowrite32(SEV_IRQ_COMPLETION, d->regs + SEV_REG_IRQ_STATUS);
    }
    return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Char dev                                                            */
/* ------------------------------------------------------------------ */
static int sev_open(struct inode *ino, struct file *f)
{
    /* Recover the per-instance struct from the cdev (not a global). */
    f->private_data = container_of(ino->i_cdev, struct sev_dev, cdev);
    return 0;
}

static int sev_mmap(struct file *f, struct vm_area_struct *vma)
{
    struct sev_dev *d = f->private_data;
    unsigned long sz = vma->vm_end - vma->vm_start;

    if (!d || !d->shmem_phys) return -ENODEV;
    if (sz > d->shmem_size)   return -EINVAL;

    /* BAR-backed shared RAM: decrypted + uncached, like ivshmem. */
    vma->vm_page_prot = pgprot_decrypted(pgprot_noncached(vma->vm_page_prot));
    return io_remap_pfn_range(vma, vma->vm_start,
                              d->shmem_phys >> PAGE_SHIFT, sz,
                              vma->vm_page_prot);
}

static long sev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct sev_dev *d = f->private_data;
    void __user *argp = (void __user *)arg;

    if (!d) return -ENODEV;

    switch (cmd) {
    case SEV_IOC_GET_SIZE: {
        u64 sz = d->shmem_size;
        return copy_to_user(argp, &sz, sizeof(sz)) ? -EFAULT : 0;
    }
    case SEV_IOC_GET_INFO: {
        struct sev_info info = {
            .vm_id = d->vm_id, .role = d->role, .shmem_size = d->shmem_size,
        };
        return copy_to_user(argp, &info, sizeof(info)) ? -EFAULT : 0;
    }
    case SEV_IOC_DOORBELL:
        if (d->role != SEV_ROLE_CLIENT) return -EPERM;
        /* Bare write to BAR0 DOORBELL; value ignored (ioeventfd). */
        iowrite32(1, d->regs + SEV_REG_DOORBELL);
        return 0;
    case SEV_IOC_WAIT_COMP: {
        int ret;
        if (d->role != SEV_ROLE_CLIENT) return -EPERM;
        ret = wait_event_interruptible(d->comp_wq,
                                       atomic_xchg(&d->comp_pending, 0));
        return ret ? -EINTR : 0;
    }
    case SEV_IOC_WAIT_RING: {
        int ret;
        if (d->role != SEV_ROLE_MANAGER) return -EPERM;
        /* Block until THIS client's doorbell rings (event-driven). */
        ret = wait_event_interruptible(d->ring_wq,
                                       atomic_xchg(&d->ring_pending, 0));
        return ret ? -EINTR : 0;
    }
    case SEV_IOC_COMPLETE: {
        /* Manager fires THIS client's completion irqfd. arg ignored: the
         * instance already identifies the target client. We still accept a
         * vm_id for API symmetry but use our own d->vm_id authoritatively. */
        if (d->role != SEV_ROLE_MANAGER) return -EPERM;
        iowrite32(d->vm_id, d->regs + SEV_REG_COMPLETE);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static const struct file_operations sev_fops = {
    .owner          = THIS_MODULE,
    .open           = sev_open,
    .mmap           = sev_mmap,
    .unlocked_ioctl = sev_ioctl,
    .compat_ioctl   = sev_ioctl,
};

/* ------------------------------------------------------------------ */
/* PCI probe / remove                                                  */
/* ------------------------------------------------------------------ */
static int sev_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct sev_dev *d;
    int ret;
    u32 magic;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) return -ENOMEM;
    d->pdev = pdev;
    d->irq  = -1;
    init_waitqueue_head(&d->comp_wq);
    init_waitqueue_head(&d->ring_wq);
    atomic_set(&d->comp_pending, 0);
    atomic_set(&d->ring_pending, 0);
    pci_set_drvdata(pdev, d);

    ret = pci_enable_device(pdev);
    if (ret) goto err_free;
    ret = pci_request_regions(pdev, DRV);
    if (ret) goto err_disable;

    d->regs = pci_iomap(pdev, SEV_BAR_REGS, SEV_BAR_REGS_SIZE);
    if (!d->regs) { ret = -ENOMEM; goto err_regions; }

    magic = ioread32(d->regs + SEV_REG_MAGIC);
    if (magic != SEV_MAGIC) {
        dev_err(&pdev->dev, "bad magic 0x%08x\n", magic);
        ret = -ENODEV; goto err_unmap;
    }
    d->role  = ioread32(d->regs + SEV_REG_ROLE);
    d->vm_id = ioread32(d->regs + SEV_REG_VM_ID);
    if (d->vm_id >= SEV_MAX_MINORS) {
        dev_err(&pdev->dev, "vm_id %u >= SEV_MAX_MINORS %u\n",
                d->vm_id, SEV_MAX_MINORS);
        ret = -EINVAL; goto err_unmap;
    }

    d->shmem_phys = pci_resource_start(pdev, SEV_BAR_SHMEM);
    d->shmem_size = pci_resource_len(pdev, SEV_BAR_SHMEM);
    if (!d->shmem_phys || !d->shmem_size) {
        dev_err(&pdev->dev, "BAR2 missing\n"); ret = -ENODEV; goto err_unmap;
    }

    /* MSI: 1 vector. Client waits on completion; manager on doorbell. */
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
    if (ret < 0) {
        dev_warn(&pdev->dev, "no MSI (%d); wait ioctls will block forever\n",
                 ret);
    } else {
        d->irq = pci_irq_vector(pdev, 0);
        ret = request_irq(d->irq, sev_irq, 0, DRV, d);
        if (ret) { dev_err(&pdev->dev, "request_irq %d\n", ret); goto err_vec; }
        d->irq_requested = true;
    }

    /* Unmask the bit THIS role cares about. */
    iowrite32(d->role == SEV_ROLE_MANAGER ? SEV_IRQ_DOORBELL
                                          : SEV_IRQ_COMPLETION,
              d->regs + SEV_REG_IRQ_MASK);

    /* Per-instance char dev: minor = vm_id -> /dev/sev_channel<vm_id>. */
    d->devt = MKDEV(MAJOR(sev_devt_base), MINOR(sev_devt_base) + d->vm_id);
    cdev_init(&d->cdev, &sev_fops);
    d->cdev.owner = THIS_MODULE;
    ret = cdev_add(&d->cdev, d->devt, 1);
    if (ret) goto err_irq;
    d->cdev_added = true;

    d->dev = device_create(sev_class, NULL, d->devt, NULL,
                           "sev_channel%u", d->vm_id);
    if (IS_ERR(d->dev)) { ret = PTR_ERR(d->dev); goto err_cdev; }

    mutex_lock(&g_devs_lock);
    g_devs[d->vm_id] = d;
    mutex_unlock(&g_devs_lock);

    dev_info(&pdev->dev,
             "sev_channel bound role=%s vm_id=%u shmem_phys=0x%llx size=%zu irq=%d -> /dev/sev_channel%u\n",
             d->role == SEV_ROLE_MANAGER ? "manager" : "client",
             d->vm_id, (u64)d->shmem_phys, d->shmem_size, d->irq, d->vm_id);
    return 0;

err_cdev:  cdev_del(&d->cdev); d->cdev_added = false;
err_irq:   if (d->irq_requested) { free_irq(d->irq, d); d->irq_requested = false; }
err_vec:   pci_free_irq_vectors(pdev);
err_unmap: pci_iounmap(pdev, d->regs);
err_regions: pci_release_regions(pdev);
err_disable: pci_disable_device(pdev);
err_free:  pci_set_drvdata(pdev, NULL); kfree(d);
    return ret;
}

static void sev_remove(struct pci_dev *pdev)
{
    struct sev_dev *d = pci_get_drvdata(pdev);
    if (!d) return;

    mutex_lock(&g_devs_lock);
    if (d->vm_id < SEV_MAX_MINORS && g_devs[d->vm_id] == d)
        g_devs[d->vm_id] = NULL;
    mutex_unlock(&g_devs_lock);

    if (d->dev)        device_destroy(sev_class, d->devt);
    if (d->cdev_added) cdev_del(&d->cdev);
    if (d->irq_requested) free_irq(d->irq, d);
    pci_free_irq_vectors(pdev);
    pci_iounmap(pdev, d->regs);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pci_set_drvdata(pdev, NULL);
    kfree(d);
}

static const struct pci_device_id sev_ids[] = {
    { PCI_DEVICE(SEV_PCI_VENDOR_ID, SEV_PCI_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, sev_ids);

static struct pci_driver sev_driver = {
    .name = DRV, .id_table = sev_ids,
    .probe = sev_probe, .remove = sev_remove,
};

static int __init sev_init(void)
{
    int ret;

    /* Contiguous minor range: one minor per possible client region. */
    ret = alloc_chrdev_region(&sev_devt_base, 0, SEV_MAX_MINORS, DRV);
    if (ret) return ret;
    sev_class = class_create(DRV);
    if (IS_ERR(sev_class)) { ret = PTR_ERR(sev_class); goto err_region; }
    ret = pci_register_driver(&sev_driver);
    if (ret) goto err_class;
    return 0;
err_class:  class_destroy(sev_class);
err_region: unregister_chrdev_region(sev_devt_base, SEV_MAX_MINORS);
    return ret;
}

static void __exit sev_exit(void)
{
    pci_unregister_driver(&sev_driver);
    class_destroy(sev_class);
    unregister_chrdev_region(sev_devt_base, SEV_MAX_MINORS);
}

module_init(sev_init);
module_exit(sev_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martin");
MODULE_DESCRIPTION("SEV channel: ioeventfd doorbell + irqfd completion (N clients)");