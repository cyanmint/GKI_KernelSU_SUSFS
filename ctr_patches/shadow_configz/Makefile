# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the shadow_configz /proc/config.gz spoof
# module.
#
# Out-of-tree build (produces shadow_configz.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build modules_install
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: copy this directory into the kernel source (e.g.
# drivers/shadow_configz/) and either add a CONFIG_SHADOW_CONFIGZ Kconfig
# entry using the `obj-$(CONFIG_SHADOW_CONFIGZ)` line below, or have the
# build inject `obj-y += shadow_configz.o` / `obj-m += shadow_configz.o`,
# mirroring how this repository injects other out-of-tree sources.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
obj-$(CONFIG_SHADOW_CONFIGZ) += shadow_configz.o
ifeq ($(CONFIG_SHADOW_CONFIGZ),)
obj-m += shadow_configz.o
endif

# shadow_configz.c #includes the generated config_gz_data.h; make sure it
# exists (and is rebuilt if the template/generator changes) before the
# object file is compiled.
$(obj)/shadow_configz.o: $(obj)/config_gz_data.h

$(obj)/config_gz_data.h: $(src)/config.template $(src)/gen_config_gz.py
	$(Q)python3 $(src)/gen_config_gz.py $(src)/config.template $@

else

# --- command-line pass (out-of-tree convenience wrapper) ---
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
PYTHON ?= python3

all: config_gz_data.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# The gzip payload embedded in shadow_configz.c is generated at build time
# from config.template so the module never depends on CONFIG_ZLIB_DEFLATE
# being available in the target kernel; regenerate whenever the template
# (or the generator script) changes.
config_gz_data.h: config.template gen_config_gz.py
	$(PYTHON) gen_config_gz.py config.template config_gz_data.h

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f config_gz_data.h

.PHONY: all modules_install clean

endif
