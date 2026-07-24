# SPDX-License-Identifier: GPL-2.0
#
# Unified kbuild Makefile for the merged lkm4ctr out-of-tree module.
#
# Builds a single lkm4ctr.ko that links the former shadow_hijack,
# shadow_ns, shadow_sysvipc, shadow_mqueue and shadow_cgdevices subsystem
# sources together, plus lkm4ctr_main.c for the module entry/exit sequence.

ifneq ($(KERNELRELEASE),)

obj-$(CONFIG_LKM4CTR) += lkm4ctr.o
ifeq ($(CONFIG_LKM4CTR),)
obj-m += lkm4ctr.o
endif

lkm4ctr-objs := \
	lkm4ctr_main.o \
	lkm4ctr_log.o \
	lkm4ctr_diagfs.o \
	lkm4ctr_hotreload.o \
	helper.sh.o \
	readme.txt.o \
	shadow_hijack/shadow_hijack.o \
	shadow_ns/shadow_ns_base.o \
	shadow_ns/shadow_ns_task.o \
	shadow_ns/shadow_ns_core.o \
	shadow_ns/shadow_ns_uts.o \
	shadow_ns/shadow_ns_pid.o \
	shadow_ns/shadow_ns_user.o \
	shadow_ns/shadow_ns_idmap.o \
	shadow_ns/shadow_ns_procfs.o \
	shadow_ns/shadow_ns_module.o \
	shadow_ns/shadow_ns_diag.o \
	vendor_ns/kernel/pid.o \
	vendor_ns/kernel/pid_namespace.o \
	vendor_ns/kernel/nsproxy.o \
	vendor_ns/kernel/user_namespace.o \
	vendor_ns/kernel/utsname.o \
	vendor_ns/fs/nsfs.o \
	vendor_ns/glue/vendor_ns_syscalls.o \
	vendor_ns/glue/vendor_ns_procfs.o \
	vendor_ns/glue/vendor_ns_diag.o \
	vendor_ns/glue/vendor_ns_module.o \
	vendor_ns/glue/vendor_ns_generic.o \
	shadow_sysvipc/shadow_sysvipc_registry.o \
	shadow_sysvipc/shadow_sysvipc_tgid.o \
	shadow_sysvipc/shadow_sysvipc_hooks.o \
	shadow_sysvipc/shadow_sysvipc_msgq.o \
	shadow_sysvipc/shadow_sysvipc_sem.o \
	shadow_sysvipc/shadow_sysvipc_shm.o \
	shadow_mqueue/shadow_mqueue_core.o \
	shadow_mqueue/shadow_mqueue_io.o \
	shadow_mqueue/shadow_mqueue_hooks.o \
	shadow_mqueue/shadow_mqueue_mount.o \
	shadow_cgdevices/shadow_cgdevices.o

ccflags-y += -I$(src)/../common

# LKM4CTR_LOG_DMESG - build-time logging switch (see common/lkm4ctr_log.h
# and lkm4ctr/Kconfig's CONFIG_LKM4CTR_LOG_DMESG for the full rationale).
# This is a standalone out-of-tree module: it is never actually built as
# part of a real kernel .config/Kconfig tree (nothing in this repository
# sources lkm4ctr/Kconfig from a top-level Kconfig -- CONFIG_LKM4CTR above
# is only ever empty in every build this Makefile is used for, so the
# obj-m fallback just above always applies), so CONFIG_LKM4CTR_LOG_DMESG
# has no real Kconfig entry point to come from either. This Makefile
# variable is the standalone-module equivalent: it defaults to "n" so
# lkm4ctr.ko is completely silent in dmesg out of the box, logging only
# to the lkm4ctr diagfs's `log` files; override via
# `make LKM4CTR_LOG_DMESG=y` to also mirror every log line to the kernel
# log, matching this module's behaviour before the diagfs/ring-buffer log
# existed.
LKM4CTR_LOG_DMESG ?= n
ifeq ($(LKM4CTR_LOG_DMESG),y)
ccflags-y += -DCONFIG_LKM4CTR_LOG_DMESG=1
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
