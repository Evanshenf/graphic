# SPDX-License-Identifier: GPL-2.0

ifneq ($(KERNELRELEASE),)

ccflags-y += -I$(src)

obj-m += graphic.o

graphic-y := graphic_drv.o \
	     memory/graphic_bo.o \
	     hardware/ls7a2000.o \
	     display/graphic_kms.o \
	     display/graphic_plane.o \
	     display/graphic_crtc.o \
	     display/graphic_output.o \
	     display/graphic_flip.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
