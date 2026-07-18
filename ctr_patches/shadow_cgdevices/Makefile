# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_cgdevices simulated cgroup device controller.
#
# Out-of-tree build (produces shadow_cgdevices.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1   # Android GKI cross build
#   make KDIR=/path/to/kernel/build modules_install
#   make KDIR=/path/to/kernel/build clean

ifneq ($(KERNELRELEASE),)

obj-$(CONFIG_SHADOW_CGDEVICES) += shadow_cgdevices.o
ifeq ($(CONFIG_SHADOW_CGDEVICES),)
obj-m += shadow_cgdevices.o
endif

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean

endif
