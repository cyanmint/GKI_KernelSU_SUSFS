# SPDX-License-Identifier: GPL-2.0-only
#
# Dual-purpose kbuild Makefile for the vendored overlay filesystem module.
#
# Unlike the other ctr_patches/shadow_* modules, this directory does not
# simulate anything: it is an unmodified-behaviour vendor copy of the real
# fs/overlayfs sources (see README.md for provenance and the one carried-over
# ctr_patches DCACHE compatibility fix), packaged so CONFIG_OVERLAY_FS can be
# loaded as `overlay.ko` on stock/production GKI boot images that were built
# with it disabled entirely (i.e. dockerd/containerd's overlay2 graphdriver
# fails with "failed to mount overlay: invalid argument" /
# "'overlay' is not supported" because the real kernel never registered the
# "overlay" filesystem type).
#
# Out-of-tree build (produces overlay.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build modules_install
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: copy this directory into the kernel source (e.g.
# fs/overlayfs/), keep/merge Kconfig, and rename the `overlay-y` object list
# below to `overlay-objs` while replacing `obj-m += overlay.o` with
# `obj-$(CONFIG_OVERLAY_FS) += overlay.o`, mirroring the upstream
# fs/overlayfs/Makefile this module is vendored from.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
#
# Deliberately always builds as an external module (obj-m), never obj-y/obj-$
# on CONFIG_OVERLAY_FS: the whole point of this directory is to load
# `overlay.ko` on top of a *running* kernel/build tree whose own .config may
# already carry `CONFIG_OVERLAY_FS=y` (e.g. a DDK/build tree used only for
# its matching vmlinux/Module.symvers, as opposed to the stripped-down
# production boot image this module actually targets) -- an obj-$(...) line
# would silently turn into an ignored obj-y entry for `make M=...` builds in
# that case instead of producing a `.ko`.
obj-m += overlay.o

overlay-y := \
	super.o \
	namei.o \
	util.o \
	inode.o \
	file.o \
	dir.o \
	readdir.o \
	copy_up.o \
	export.o

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
