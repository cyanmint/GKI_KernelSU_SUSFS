# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_ctr_checker out-of-tree module.
#
# shadow_ctr_checker.ko is a pure, standalone diagnostics tool: it has NO
# build-time (KBUILD_EXTRA_SYMBOLS/Module.symvers) or load-time dependency on
# any other shadow_ctr module (shadow_ns, shadow_mqueue, shadow_sysvipc,
# shadow_cgdevices, shadow_hijack, shadow_overlay2, ...). It can be built and
# insmod'd entirely on its own, in any order. (An earlier revision took a
# hard dependency on shadow_ns's EXPORT_SYMBOL_GPL query API, and before that
# used symbol_get()/symbol_put() to detect modules purely at runtime - but
# __symbol_get()/__symbol_put() are themselves trimmed from production GKI
# kernels' exported-symbol table, which made insmod of this module fail
# unconditionally with "Unknown symbol __symbol_get"/-ENOENT. See the comment
# at the top of shadow_ctr_checker.c for the full history and rationale.)
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
