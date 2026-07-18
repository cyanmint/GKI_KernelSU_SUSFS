# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_ns_uts out-of-tree module.
#
# shadow_ns_uts.ko is a per-type extension of shadow_ns_base.ko: it registers
# the UTS namespace type (real nodename/domainname support) through the plugin
# API in common/shadow_ns_base.h, and also hooks syscalls directly via
# shadow_hook_install()/remove() (implementation in ../shadow_hijack/). It
# therefore references symbols exported by BOTH shadow_ns_base.ko and
# shadow_hijack.ko and must resolve them at modpost time via
# KBUILD_EXTRA_SYMBOLS, which defaults below to the sibling shadow_ns_base and
# shadow_hijack builds' Module.symvers (KBUILD_EXTRA_SYMBOLS accepts a
# space-separated list of files). Build (and load) shadow_hijack and
# shadow_ns_base first.
#
# Out-of-tree build (produces shadow_ns_uts.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules           # from this dir
#   make KDIR=/path/to/kernel/build                          # convenience wrapper
#   make KDIR=/path/to/kernel/build \
#        KBUILD_EXTRA_SYMBOLS="/abs/path/to/shadow_ns_base/Module.symvers /abs/path/to/shadow_hijack/Module.symvers"
#
# The default assumes shadow_ns_base/ and shadow_hijack/ are sibling
# directories that have already been built. Override KBUILD_EXTRA_SYMBOLS to
# point elsewhere if needed.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
obj-$(CONFIG_SHADOW_NS_UTS) += shadow_ns_uts.o
ifeq ($(CONFIG_SHADOW_NS_UTS),)
obj-m += shadow_ns_uts.o
endif

# shadow_hook.h + plugin API header live in ../common; the shared namespace
# type/UTS layout header lives under ../shadow_ns_base (included as
# "include/uapi/shadow_ns.h").
ccflags-y += -I$(src)/../common -I$(src)/../shadow_ns_base

else

# --- command-line pass (out-of-tree convenience wrapper) ---
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
KBUILD_EXTRA_SYMBOLS ?= $(realpath $(PWD)/../shadow_ns_base/Module.symvers) $(realpath $(PWD)/../shadow_hijack/Module.symvers)
export KBUILD_EXTRA_SYMBOLS

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean

endif
