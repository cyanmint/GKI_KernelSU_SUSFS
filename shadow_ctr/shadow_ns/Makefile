# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the standalone shadow_ns module.
#
# shadow_ns.ko is a single, self-contained namespace system (i.e. no
# plugin/submodule architecture): it hooks
# unshare(2)/setns(2)/clone(2)/clone3(2)/fork(2)/vfork(2) and, for whichever
# namespace types this kernel build genuinely lacks (per IS_ENABLED(CONFIG_
# {UTS,IPC,USER,PID,NET}_NS) — which also collapses correctly to "all five
# absent" on a kernel built with CONFIG_NAMESPACES=n entirely, since those
# five Kconfig symbols live inside "if NAMESPACES ... endif" in init/Kconfig),
# takes over that type's namespace handling completely instead of leaving it
# a stub. See README.md for the full design/scope. It has no build-time or
# load-time dependency on any other shadow_ns_* module — those (shadow_ns_base,
# shadow_ns_uts, shadow_ns_ipc, shadow_ns_mnt, shadow_ns_pid, shadow_ns_net,
# shadow_ns_user, shadow_ns_cgroup) have been removed.
#
# shadow_ns hooks syscalls via shadow_hook_install()/remove(), whose
# implementation lives in the standalone shadow_hijack.ko (see
# ../shadow_hijack/). It therefore resolves shadow_hijack's EXPORT_SYMBOL_GPL
# symbols at modpost time via KBUILD_EXTRA_SYMBOLS, which defaults below to
# the sibling shadow_hijack build's Module.symvers. Build (and load)
# shadow_hijack first.
#
# Out-of-tree build (produces shadow_ns.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: add a CONFIG_SHADOW_NS Kconfig entry and rely on the
# obj-$(CONFIG_SHADOW_NS) line below, or inject obj-m manually.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
obj-$(CONFIG_SHADOW_NS) += shadow_ns.o
ifeq ($(CONFIG_SHADOW_NS),)
obj-m += shadow_ns.o
endif

# shadow_hook.h (used to hook unshare/setns/clone/clone3/fork/vfork) lives
# in ../common, same as every other shadow_ctr hooking module.
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
