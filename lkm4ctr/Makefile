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
	lkm4ctr_safe_unload.o \
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
