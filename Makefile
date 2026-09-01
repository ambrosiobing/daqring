# SPDX-License-Identifier: GPL-2.0
# Classic dual-purpose kbuild Makefile: invoked once from the command
# line, then re-read by the kernel build system with KERNELRELEASE set.

ifneq ($(KERNELRELEASE),)

obj-m := daqring.o
ccflags-y := -I$(src)/include

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: module test/daqring_test

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test/daqring_test: test/daqring_test.c include/daqring.h
	$(CC) -O2 -Wall -Wextra -Iinclude -o $@ test/daqring_test.c

dtbo: daqring.dtbo

daqring.dtbo: overlays/daqring-overlay.dts
	dtc -@ -I dts -O dtb -o $@ overlays/daqring-overlay.dts

# Bookworm and later mount the boot partition at /boot/firmware;
# older releases use /boot.
BOOTDIR := $(shell test -d /boot/firmware && echo /boot/firmware || echo /boot)

install-overlay: daqring.dtbo
	sudo mkdir -p $(BOOTDIR)/overlays
	sudo cp daqring.dtbo $(BOOTDIR)/overlays/
	@grep -q '^dtoverlay=daqring' $(BOOTDIR)/config.txt || \
		echo 'dtoverlay=daqring' | sudo tee -a $(BOOTDIR)/config.txt
	@echo "overlay installed in $(BOOTDIR) - reboot to apply"

load: module
	sudo insmod daqring.ko

unload:
	sudo rmmod daqring

demo: all
	./scripts/demo.sh

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test/daqring_test daqring.dtbo

.PHONY: all module dtbo install-overlay load unload demo clean

endif
