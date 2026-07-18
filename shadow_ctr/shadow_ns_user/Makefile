# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_ns_user out-of-tree module.
#
# shadow_ns_user.ko is a thin per-type extension of shadow_ns_base.ko: it
# claims the SHADOW_NS_TYPE_USER plugin slot through the API in
# common/shadow_ns_base.h. It references symbols exported by shadow_ns_base.ko
# and must resolve them at modpost time via KBUILD_EXTRA_SYMBOLS, which
# defaults to the sibling shadow_ns_base build's Module.symvers. Build (and
# load) shadow_ns_base first.
#
# Out-of-tree build (produces shadow_ns_user.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build \
#        KBUILD_EXTRA_SYMBOLS=/abs/path/to/shadow_ns_base/Module.symvers

ifneq ($(KERNELRELEASE),)

# --- kbuild pass ---
obj-$(CONFIG_SHADOW_NS_USER) += shadow_ns_user.o
ifeq ($(CONFIG_SHADOW_NS_USER),)
obj-m += shadow_ns_user.o
endif

ccflags-y += -I$(src)/../common -I$(src)/../shadow_ns_base

else

# --- command-line pass (out-of-tree convenience wrapper) ---
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
KBUILD_EXTRA_SYMBOLS ?= $(realpath $(PWD)/../shadow_ns_base/Module.symvers)
export KBUILD_EXTRA_SYMBOLS

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean

endif
