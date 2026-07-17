# sev_gpu multi-file module build.
# As each cluster is extracted, add its .o here. Order doesn't matter for the
# link, but every extracted .c must be listed or its symbols go undefined.
obj-m += sev_gpu.o
sev_gpu-objs := \
	sev_gpu_manager.o \
	sev_gpu_crypto.o \
	sev_gpu_kmb.o \
	sev_gpu_handshake.o \
	sev_gpu_xport_channel.o \
	sev_gpu_client_mmap.o \
	sev_gpu_client_rm.o \
	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o
# Transport backend: EXACTLY ONE of xport_channel.o / xport_ivshmem.o may be
# linked (they define the same symbols). Default = channel (plain+ioeventfd+irqfd).
# To fall back to ivshmem-doorbell during bring-up, swap the line above.
#	sev_gpu_comm.o          # (pending) RPC framing/layout
#	sev_gpu_kmb.o \
	sev_gpu_handshake.o \
	sev_gpu_xport_channel.o \
	sev_gpu_xport_ivshmem.o \
	sev_gpu_client_mmap.o \
	sev_gpu_client_rm.o \
	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o           # (pending) KMB seal/install/send/recv
#	sev_gpu_handshake.o \
	sev_gpu_xport_channel.o \
	sev_gpu_xport_ivshmem.o \
	sev_gpu_client_mmap.o \
	sev_gpu_client_rm.o \
	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o     # (pending) ECDHE-PSK exchange + comm key
#	sev_gpu_xport_channel.o \
	sev_gpu_xport_ivshmem.o \
	sev_gpu_client_mmap.o \
	sev_gpu_client_rm.o \
	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o # (pending) transport backend (swap point)
#	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o # (pending)
#	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o  # (pending)
#	sev_gpu_manager_nvidia.o# (pending)
#	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o       # (pending)
#	sev_gpu_client_rm.o \
	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o     # (pending)
#	sev_gpu_client_mmap.o \
	sev_gpu_client_rm.o \
	sev_gpu_manager_exec.o \
	sev_gpu_manager_sched.o \
	sev_gpu_bringup.o \
	sev_gpu_chardev.o \
	sev_gpu_manager_mmap.o   # (pending)
#	sev_gpu_client_gpu.o    # (pending)
#	sev_gpu_ioctl.o         # (pending)

KDIR ?= /lib/modules/$(shell uname -r)/build
ccflags-y := -I$(src)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean