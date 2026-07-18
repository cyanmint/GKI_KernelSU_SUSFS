# SPDX-License-Identifier: GPL-2.0
#
# Dual-purpose kbuild Makefile for the combined shadow_ctr module.
#
# shadow_ctr.ko links together five independent subsystems (each still
# separately documented — see the READMEs listed below) into a single
# out-of-tree module:
#
#   shadow_ns.c          - shadow_ns.README.md      (simulated namespaces)
#   shadow_sysvipc.c      - shadow_sysvipc.README.md  (simulated SysV IPC)
#   shadow_mqueue.c        - shadow_mqueue.README.md   (simulated POSIX mqueue)
#   shadow_cgdevices.c    - shadow_cgdevices.README.md (cgroup device enforcement)
#   shadow_configspoof.c  - shadow_configspoof.README.md (/proc/config.gz spoof)
#   shadow_ctr_main.c     - module-wide metadata / combined banner only
#
# Out-of-tree build (produces shadow_ctr.ko):
#   make KDIR=/path/to/kernel/build
#   make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
#   make KDIR=/path/to/kernel/build modules_install
#   make KDIR=/path/to/kernel/build clean
#
# In-tree build: copy this directory into the kernel source (e.g.
# drivers/shadow_ctr/) and either add a CONFIG_SHADOW_CTR Kconfig entry using
# the `obj-$(CONFIG_SHADOW_CTR)` line below, or have the build inject
# `obj-y += shadow_ctr.o` / `obj-m += shadow_ctr.o`, mirroring how this
# repository injects other out-of-tree sources.

ifneq ($(KERNELRELEASE),)

# --- kbuild pass (invoked from within the kernel build system) ---
obj-$(CONFIG_SHADOW_CTR) += shadow_ctr.o
ifeq ($(CONFIG_SHADOW_CTR),)
obj-m += shadow_ctr.o
endif

shadow_ctr-y := \
	shadow_ctr_main.o \
	shadow_ns.o \
	shadow_sysvipc.o \
	shadow_mqueue.o \
	shadow_cgdevices.o \
	shadow_configspoof.o

# shadow_configspoof.c #includes the generated config_gz_data.h; make sure it
# exists (and is rebuilt if the template/generator change) before that object
# file is compiled.
$(obj)/shadow_configspoof.o: $(obj)/config_gz_data.h

$(obj)/config_gz_data.h: $(src)/config.template $(src)/gen_config_gz.py
	$(Q)python3 $(src)/gen_config_gz.py $(src)/config.template $@

else

# --- command-line pass (out-of-tree convenience wrapper) ---
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
PYTHON ?= python3

all: config_gz_data.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

config_gz_data.h: config.template gen_config_gz.py
	$(PYTHON) gen_config_gz.py config.template config_gz_data.h

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f config_gz_data.h

.PHONY: all modules_install clean

endif
