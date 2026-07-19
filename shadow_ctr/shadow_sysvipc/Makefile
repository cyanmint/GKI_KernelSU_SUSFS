# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the standalone shadow_sysvipc out-of-tree module.
#
# shadow_sysvipc.ko has no build-time or load-time dependency on any other shadow_ctr
# per-subsystem module, but it DOES hook syscalls via shadow_hook_install()/
# remove(), whose implementation lives in the standalone shadow_hijack.ko
# (see ../shadow_hijack/). It therefore resolves shadow_hijack's
# EXPORT_SYMBOL_GPL symbols at modpost time via KBUILD_EXTRA_SYMBOLS, which
# defaults below to the sibling shadow_hijack build's Module.symvers. Build
# (and load) shadow_hijack first. Aside from that, it shares only the
# header-only helpers in ../common (shadow_hook.h, and for shadow_mqueue the
# fd_file/fd_empty compat shims in shadow_ctr_compat.h).
#
# Out-of-tree build (produces shadow_sysvipc.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules
#   make KDIR=/path/to/kernel/build

ifneq ($(KERNELRELEASE),)

# --- kbuild pass ---
obj-$(CONFIG_SHADOW_SYSVIPC) += shadow_sysvipc.o
shadow_sysvipc-objs := shadow_sysvipc_registry.o shadow_sysvipc_tgid.o shadow_sysvipc_hooks.o
ifeq ($(CONFIG_SHADOW_SYSVIPC),)
obj-m += shadow_sysvipc.o
endif

# shadow_hook.h and shadow_ctr_compat.h live in ../common.
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
