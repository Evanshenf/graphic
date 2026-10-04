# SPDX-License-Identifier: GPL-2.0

ifneq ($(KERNELRELEASE),)

ccflags-y += -I$(src)

obj-m += graphic.o

graphic-y := lsdc_module.o \
	     lsdc_pci.o \
	     lsdc_driver.o \
	     soc/lsdc_gmch.o \
	     lsdc_region_ttm.o \
	     lsdc_gem.o \
	     gem/lsdc_gem_ttm.o \
	     display/lsdc_display_driver.o \
	     display/lsdc_plane.o \
	     display/lsdc_crtc.o \
	     display/lsdc_output.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
