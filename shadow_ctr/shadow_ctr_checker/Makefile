# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_ctr_checker out-of-tree module.
#
# shadow_ctr_checker.ko has NO build-time dependency on any other shadow_ctr
# module: it detects the other modules purely at RUNTIME via symbol_get()
# (see shadow_ctr_checker.c). Therefore it deliberately does NOT set
# KBUILD_EXTRA_SYMBOLS and does not need any sibling module's Module.symvers to
# build or load. It only needs a compile-time include path to shadow_ns_base's
# UAPI header for the SHADOW_NS_TYPE_* enum values.
#
# Out-of-tree build (produces shadow_ctr_checker.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules
#   make KDIR=/path/to/kernel/build

ifneq ($(KERNELRELEASE),)

# --- kbuild pass ---
obj-$(CONFIG_SHADOW_CTR_CHECKER) += shadow_ctr_checker.o
ifeq ($(CONFIG_SHADOW_CTR_CHECKER),)
obj-m += shadow_ctr_checker.o
endif

# Only a header include path (for include/uapi/shadow_ns.h); NOT a symbol dep.
ccflags-y += -I$(src)/../shadow_ns_base

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
