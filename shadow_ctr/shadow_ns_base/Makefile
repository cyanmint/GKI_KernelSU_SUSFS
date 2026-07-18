# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_ns_base out-of-tree module.
#
# shadow_ns_base.ko is the generic shadow-namespace registry + syscall-hook
# module. It exports the plugin API (common/shadow_ns_base.h) that the per-type
# submodules (shadow_ns_uts, shadow_ns_net, shadow_ns_ipc, shadow_ns_pid,
# shadow_ns_mnt, shadow_ns_user, shadow_ns_cgroup) register with — so it must
# be built (and, at load time, inserted) before any of them.
#
# shadow_ns_base itself hooks syscalls via shadow_hook_install()/remove(),
# whose implementation lives in the standalone shadow_hijack.ko (see
# ../shadow_hijack/). It therefore resolves shadow_hijack's EXPORT_SYMBOL_GPL
# symbols at modpost time via KBUILD_EXTRA_SYMBOLS, which defaults below to
# the sibling shadow_hijack build's Module.symvers. Build (and load)
# shadow_hijack first.
#
# Out-of-tree build (produces shadow_ns_base.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: add a CONFIG_SHADOW_NS_BASE Kconfig entry and rely on the
# obj-$(CONFIG_SHADOW_NS_BASE) line below, or inject obj-m manually.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
obj-$(CONFIG_SHADOW_NS_BASE) += shadow_ns_base.o
ifeq ($(CONFIG_SHADOW_NS_BASE),)
obj-m += shadow_ns_base.o
endif

# shadow_hook.h and the plugin API header live in ../common (shared, header-only).
ccflags-y += -I$(src)/../common

else

# --- command-line pass (out-of-tree convenience wrapper) ---
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
KBUILD_EXTRA_SYMBOLS ?= $(realpath $(PWD)/../shadow_hijack/Module.symvers)
export KBUILD_EXTRA_SYMBOLS

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean

endif
