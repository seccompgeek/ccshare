/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sev_gpu_chardev.h — character-device interface: /dev/sev_gpu_manager (control,
 * minor 0) and /dev/sev_gpu_dataN (per-VM DATA regions). open/release/mmap +
 * fops + cdev setup/teardown. The mmap handlers map BAR2 with pgprot_decrypted
 * (C-bit clear) so both CVMs share it (ivshmem-style).
 *
 * The ioctl DISPATCH (sev_gpu_ioctl) lives in sev_gpu_main.c and is declared
 * here so the control fops can reference it.
 */
#ifndef SEV_GPU_CHARDEV_H
#define SEV_GPU_CHARDEV_H

#include <linux/fs.h>
#include <linux/cdev.h>

#define DEVICE_NAME "sev_gpu_manager"


struct sev_gpu_dev;
struct sev_gpu_data_dev;

/* shared chardev identity (defined in sev_gpu_main.c). */
extern struct class *sev_gpu_class;
extern dev_t sev_gpu_devt_base;

/* main ioctl dispatch (defined in sev_gpu_main.c). */
long sev_gpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

int  sev_gpu_setup_chardev(struct sev_gpu_dev *d);
void sev_gpu_teardown_chardev(struct sev_gpu_dev *d);
void data_init_header(struct sev_gpu_data_dev *dd);
int  sev_gpu_data_setup_chardev(struct sev_gpu_data_dev *dd);
void sev_gpu_data_teardown_chardev(struct sev_gpu_data_dev *dd);

#endif /* SEV_GPU_CHARDEV_H */
