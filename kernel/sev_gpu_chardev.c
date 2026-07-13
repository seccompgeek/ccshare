// SPDX-License-Identifier: GPL-2.0
/*
 * sev_gpu_chardev.c — char-device interface (control + data devices).
 * mmap handlers map BAR2 with pgprot_decrypted (C-bit clear, ivshmem-style).
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>
#include "sev_gpu_manager.h"
#include "sev_gpu_regions.h"
#include "sev_gpu_state.h"
#include "sev_gpu_chardev.h"

static int sev_gpu_open(struct inode *inode, struct file *filp)
{
	filp->private_data = ctrl_dev;
	return 0;
}

static int sev_gpu_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * Map the shared CONTROL region (BAR2) into user space. This region holds only
 * scheduling metadata (header + request/grant rings); GPU-work payload lives in
 * the separate, hardware-isolated per-VM data devices, so there is nothing
 * sensitive to bound here.
 */
static int sev_gpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sev_gpu_dev *d = filp->private_data;
	unsigned long vsize = vma->vm_end - vma->vm_start;

	if (!d || !d->shmem_phys)
		return -ENODEV;
	if (vsize > d->shmem_size)
		return -EINVAL;

	/* BAR is shared (unencrypted) MMIO-backed RAM: map decrypted + uncached. */
	vma->vm_page_prot = pgprot_decrypted(pgprot_noncached(vma->vm_page_prot));

	return io_remap_pfn_range(vma, vma->vm_start,
				  d->shmem_phys >> PAGE_SHIFT,
				  vsize, vma->vm_page_prot);
}

static const struct file_operations sev_gpu_fops = {
	.owner          = THIS_MODULE,
	.open           = sev_gpu_open,
	.release        = sev_gpu_release,
	.mmap           = sev_gpu_mmap,
	.unlocked_ioctl = sev_gpu_ioctl,
	.compat_ioctl   = sev_gpu_ioctl,
};

int sev_gpu_setup_chardev(struct sev_gpu_dev *d)
{
	int ret;

	/* Control device uses minor 0 of the shared region + the shared class. */
	d->devt = MKDEV(MAJOR(sev_gpu_devt_base), MINOR(sev_gpu_devt_base));
	cdev_init(&d->cdev, &sev_gpu_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret)
		return ret;

	d->device = device_create(sev_gpu_class, NULL, d->devt, NULL, DEVICE_NAME);
	if (IS_ERR(d->device)) {
		ret = PTR_ERR(d->device);
		cdev_del(&d->cdev);
		return ret;
	}
	return 0;
}

void sev_gpu_teardown_chardev(struct sev_gpu_dev *d)
{
	device_destroy(sev_gpu_class, d->devt);
	cdev_del(&d->cdev);
}

/* Manager: initialise a fresh data-region header (identity binding). */
void data_init_header(struct sev_gpu_data_dev *dd)
{
	sev_gpu_data_header_t h;

	memset(&h, 0, sizeof(h));
	h.magic        = SEV_GPU_DATA_MAGIC;
	h.version      = SEV_GPU_DATA_VERSION;
	h.pool_index   = dd->pool_index;
	h.owner_vm_id  = dd->pool_index;	/* default identity binding */
	h.state        = SEV_GPU_DATA_BOUND;
	h.region_size  = dd->mem_size;
	h.payload_off  = SEV_GPU_DATA_HEADER_SIZE;
	h.payload_size = (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE) ?
				dd->mem_size - SEV_GPU_DATA_HEADER_SIZE : 0;
	memcpy_toio(dd->mem, &h, sizeof(h));
}

static int sev_gpu_data_open(struct inode *inode, struct file *filp)
{
	filp->private_data = container_of(inode->i_cdev,
					  struct sev_gpu_data_dev, cdev);
	return 0;
}

static int sev_gpu_data_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* Map the WHOLE private region (header + payload) -- already isolated by QEMU. */
static int sev_gpu_data_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sev_gpu_data_dev *dd = filp->private_data;
	unsigned long vsize = vma->vm_end - vma->vm_start;

	if (!dd || !dd->mem_phys)
		return -ENODEV;
	if (vsize > dd->mem_size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_decrypted(pgprot_noncached(vma->vm_page_prot));
	return io_remap_pfn_range(vma, vma->vm_start,
				  dd->mem_phys >> PAGE_SHIFT,
				  vsize, vma->vm_page_prot);
}

static long sev_gpu_data_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct sev_gpu_data_dev *dd = filp->private_data;
	void __user *argp = (void __user *)arg;
	sev_gpu_data_header_t h;

	if (!dd)
		return -ENODEV;

	switch (cmd) {
	case SEV_GPU_IOC_DATA_INFO: {
		sev_gpu_ioctl_data_info_t info;

		memcpy_fromio(&h, dd->mem, sizeof(h));
		memset(&info, 0, sizeof(info));
		info.pool_index   = dd->pool_index;
		info.is_manager   = dd->is_manager ? 1 : 0;
		info.region_size  = dd->mem_size;
		info.payload_off  = SEV_GPU_DATA_HEADER_SIZE;
		info.payload_size = (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE) ?
					dd->mem_size - SEV_GPU_DATA_HEADER_SIZE : 0;
		if (h.magic == SEV_GPU_DATA_MAGIC) {
			info.owner_vm_id = h.owner_vm_id;
			info.state       = h.state;
		} else {
			info.owner_vm_id = SEV_GPU_DATA_OWNER_NONE;
			info.state       = SEV_GPU_DATA_FREE;
		}
		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		break;
	}

	case SEV_GPU_IOC_DATA_BIND: {
		sev_gpu_ioctl_data_bind_t bind;

		if (!dd->is_manager)
			return -EPERM;	/* only the manager (re)binds regions */
		if (copy_from_user(&bind, argp, sizeof(bind)))
			return -EFAULT;

		/* Scrub the payload before changing ownership so a reused
		 * region can't leak the previous owner's bytes. */
		if (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE)
			memset_io(dd->mem + SEV_GPU_DATA_HEADER_SIZE, 0,
				  dd->mem_size - SEV_GPU_DATA_HEADER_SIZE);

		memcpy_fromio(&h, dd->mem, sizeof(h));
		if (h.magic != SEV_GPU_DATA_MAGIC) {
			memset(&h, 0, sizeof(h));
			h.magic        = SEV_GPU_DATA_MAGIC;
			h.version      = SEV_GPU_DATA_VERSION;
			h.pool_index   = dd->pool_index;
			h.region_size  = dd->mem_size;
			h.payload_off  = SEV_GPU_DATA_HEADER_SIZE;
			h.payload_size = (dd->mem_size > SEV_GPU_DATA_HEADER_SIZE) ?
						dd->mem_size - SEV_GPU_DATA_HEADER_SIZE : 0;
		}
		h.owner_vm_id = bind.owner_vm_id;
		h.state = (bind.owner_vm_id == SEV_GPU_DATA_OWNER_NONE) ?
				SEV_GPU_DATA_FREE : SEV_GPU_DATA_BOUND;
		h.data_len = 0;
		h.seq = 0;
		memcpy_toio(dd->mem, &h, sizeof(h));

		pr_info("sev_gpu: data[%u] bound to owner %u\n",
			dd->pool_index, bind.owner_vm_id);
		break;
	}

	default:
		return -ENOTTY;
	}
	return 0;
}

static const struct file_operations sev_gpu_data_fops = {
	.owner          = THIS_MODULE,
	.open           = sev_gpu_data_open,
	.release        = sev_gpu_data_release,
	.mmap           = sev_gpu_data_mmap,
	.unlocked_ioctl = sev_gpu_data_ioctl,
	.compat_ioctl   = sev_gpu_data_ioctl,
};

int sev_gpu_data_setup_chardev(struct sev_gpu_data_dev *dd)
{
	int ret;

	dd->devt = MKDEV(MAJOR(sev_gpu_devt_base),
			 MINOR(sev_gpu_devt_base) + 1 + dd->pool_index);
	cdev_init(&dd->cdev, &sev_gpu_data_fops);
	dd->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dd->cdev, dd->devt, 1);
	if (ret)
		return ret;

	dd->device = device_create(sev_gpu_class, NULL, dd->devt, NULL,
				   "sev_gpu_data%u", dd->pool_index);
	if (IS_ERR(dd->device)) {
		ret = PTR_ERR(dd->device);
		cdev_del(&dd->cdev);
		return ret;
	}
	return 0;
}

void sev_gpu_data_teardown_chardev(struct sev_gpu_data_dev *dd)
{
	device_destroy(sev_gpu_class, dd->devt);
	cdev_del(&dd->cdev);
}