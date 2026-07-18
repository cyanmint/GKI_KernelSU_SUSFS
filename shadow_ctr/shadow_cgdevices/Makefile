# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the standalone shadow_cgdevices out-of-tree module.
#
# shadow_cgdevices.ko is fully self-contained: it has no build-time or load-time
# dependency on any other shadow_ctr module. It shares only the header-only
# helpers in ../common (shadow_hook.h, and for shadow_mqueue the fd_file/
# fd_empty compat shims in shadow_ctr_compat.h).
#
# Out-of-tree build (produces shadow_cgdevices.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules
#   make KDIR=/path/to/kernel/build

ifneq ($(KERNELRELEASE),)

# --- kbuild pass ---
obj-$(CONFIG_SHADOW_CGDEVICES) += shadow_cgdevices.o
ifeq ($(CONFIG_SHADOW_CGDEVICES),)
obj-m += shadow_cgdevices.o
endif

# shadow_hook.h and shadow_ctr_compat.h live in ../common.
ccflags-y += -I$(src)/../common

else

# --- command-line pass (out-of-tree convenience wrapper) ---
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
