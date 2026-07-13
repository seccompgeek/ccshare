obj-m += sev_gpu_manager.o

# Build against the pinned 6.16 SNP-guest headers vendored under sev-changes/
# (extracted from linux-headers-6.16.0-snp-guest...deb) when present, so the
# host's running kernel doesn't dictate the target. Override with `make KDIR=...`.
GUEST_KDIR := $(wildcard $(CURDIR)/../kdir-6.16-guest/usr/src/linux-headers-*)
ifeq ($(GUEST_KDIR),)
KDIR ?= /lib/modules/$(shell uname -r)/build
else
KDIR ?= $(GUEST_KDIR)
endif
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

help:
	$(MAKE) -C $(KDIR) M=$(PWD) help
