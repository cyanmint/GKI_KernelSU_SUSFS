# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadowns simulated namespace module.
#
# Out-of-tree build (produces shadowns.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build modules_install
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: copy this directory into the kernel source (e.g.
# drivers/shadowns/) and either add a CONFIG_SHADOWNS Kconfig entry using the
# `obj-$(CONFIG_SHADOWNS)` line below, or have the build inject
# `obj-y += shadowns.o` / `obj-m += shadowns.o`, mirroring how this repository
# injects other out-of-tree sources (see drivers/.../hmbird_patch.o).

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
# In-tree, CONFIG_SHADOWNS selects the module; for out-of-tree builds
# CONFIG_SHADOWNS is unset, so fall back to building it as a module.
obj-$(CONFIG_SHADOWNS) += shadowns.o
ifeq ($(CONFIG_SHADOWNS),)
obj-m += shadowns.o
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
