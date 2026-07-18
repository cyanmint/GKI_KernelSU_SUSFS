# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the standalone shadow_overlay2 module.
#
# shadow_overlay2.ko is the genuine, vendored-from-upstream fs/overlayfs
# ("overlay" filesystem type) — NOT a simulation. It registers a working
# "overlay" filesystem for GKI images shipped without CONFIG_OVERLAY_FS.
#
# IMPORTANT: this module only compiles against the android14-6.1 kernel
# branch. Other KMIs have incompatible vfs_tmpfile_open()/lookup_one()/
# mnt_idmap ABIs. See README.md.
#
# The .ko is a multi-object module: obj-m names the final module
# (shadow_overlay2), and shadow_overlay2-y lists the constituent object files
# under overlay/. It has no build-time or load-time dependency on any other
# shadow_ctr module.
#
# Out-of-tree build (produces shadow_overlay2.ko):
#   make -C /path/to/kernel/build M=$(pwd) modules
#   make KDIR=/path/to/kernel/build

ifneq ($(KERNELRELEASE),)

# --- kbuild pass ---
obj-$(CONFIG_SHADOW_OVERLAY2) += shadow_overlay2.o
ifeq ($(CONFIG_SHADOW_OVERLAY2),)
obj-m += shadow_overlay2.o
endif

shadow_overlay2-y := \
	overlay/super.o \
	overlay/namei.o \
	overlay/util.o \
	overlay/inode.o \
	overlay/file.o \
	overlay/dir.o \
	overlay/readdir.o \
	overlay/copy_up.o \
	overlay/export.o

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
