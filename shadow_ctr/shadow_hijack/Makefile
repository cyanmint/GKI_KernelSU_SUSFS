# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_hijack out-of-tree module.
#
# shadow_hijack.ko is the shared function-hijacking implementation: it
# EXPORT_SYMBOL_GPL()s the shadow_hook_install/remove/resolve/install_all/
# remove_all entry points declared in common/shadow_hook.h. Every hooking
# module (shadow_ns, shadow_sysvipc, shadow_mqueue,
# shadow_cgdevices) resolves those symbols at modpost time via
# KBUILD_EXTRA_SYMBOLS=<shadow_hijack>/Module.symvers, so it must be built
# (and, at load time, inserted) before any of them.
#
# Out-of-tree build (produces shadow_hijack.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: add a CONFIG_SHADOW_HIJACK Kconfig entry and rely on the
# obj-$(CONFIG_SHADOW_HIJACK) line below, or inject obj-m manually.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
obj-$(CONFIG_SHADOW_HIJACK) += shadow_hijack.o
ifeq ($(CONFIG_SHADOW_HIJACK),)
obj-m += shadow_hijack.o
endif

# shadow_hook.h (the ABI this module implements) lives in ../common.
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
