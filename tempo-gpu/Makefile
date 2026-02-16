
# SPDX-License-Identifier: GPL-2.0
#
# Tempo GPU: Workload-Aware Temporal GPU Scheduler
# Kernel module Makefile
#

obj-m += tempo-gpu.o

tempo-gpu-y := tempo_mdev.o \
               tempo_scheduler.o \
               tempo_runlist.o \
               tempo_tsg.o \
               tempo_vram.o \
               tempo_idle.o \
               tempo_debugfs.o \
               tempo_mmio.o

# Extra compiler flags
ccflags-y += -Wall -Werror -Wno-unused-function

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install into /lib/modules/.../extra/
install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Load with dependencies
load:
	modprobe vfio
	modprobe vfio-pci
	modprobe vfio-iommu-type1
	modprobe mdev
	insmod tempo-gpu.ko

unload:
	rmmod tempo-gpu 2>/dev/null || true

reload: unload load

.PHONY: all clean install load unload reload
