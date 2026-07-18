# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_ctr_checker out-of-tree module.
#
# shadow_ctr_checker.ko has a build+load-time dependency on shadow_ns_base.ko
# only (for its namespace bookkeeping query API, resolved as ordinary
# EXPORT_SYMBOL_GPL calls; see shadow_ctr_checker.c). It deliberately does NOT
# depend on shadow_mqueue/shadow_sysvipc/shadow_cgdevices/shadow_overlay2:
# those are optional and independently loadable, and a hard Kbuild dependency
# on them would make this checker fail to insmod whenever one of them isn't
# loaded. (An earlier revision used symbol_get()/symbol_put() to detect all of
# them, including shadow_ns_base, purely at runtime with zero Kbuild
# dependencies - but __symbol_get()/__symbol_put() are themselves trimmed from
# production GKI kernels' exported-symbol table, which made insmod of this
# module fail unconditionally with "Unknown symbol __symbol_get"/-ENOENT. See
# the comment at the top of shadow_ctr_checker.c for details.)
#
# shadow_ns_base is foundational and always loaded first (right after
# shadow_hijack) per ../README.md's load order, so this dependency does not
# reduce standalone usability in practice - matching the same
# KBUILD_EXTRA_SYMBOLS pattern shadow_ns_uts/net/... already use.
#
# Out-of-tree build (produces shadow_ctr_checker.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build \
#        KBUILD_EXTRA_SYMBOLS="/abs/path/to/shadow_ns_base/Module.symvers"
#
# The default assumes shadow_ns_base/ is a sibling directory that has already
# been built. Override KBUILD_EXTRA_SYMBOLS to point elsewhere if needed.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass ---
obj-$(CONFIG_SHADOW_CTR_CHECKER) += shadow_ctr_checker.o
ifeq ($(CONFIG_SHADOW_CTR_CHECKER),)
obj-m += shadow_ctr_checker.o
endif

# Header include path (for include/uapi/shadow_ns.h) and the symbol
# dependency on shadow_ns_base's EXPORT_SYMBOL_GPL query API.
ccflags-y += -I$(src)/../shadow_ns_base

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
