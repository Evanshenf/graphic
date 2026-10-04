# SPDX-License-Identifier: GPL-2.0


subdir-ccflags-y += -I/home/virgo/graphic

loongson-y += lsdc_driver.o \
	lsdc_pci.o \
	lsdc_params.o \
	lsdc_mitigations.o \
	lsdc_module.o \
	lsdc_region_ttm.o \
	lsdc_gem.o \



gem-y := gem/lsdc_gem_ttm.o
loongson-y += \
    $(gem-y)

#modesetting core code

loongson-y +=\
	display/lsdc_display_driver.o \
	display/lsdc_vga.o \

loongson-y +=\
	soc/lsdc_gmch.o

obj-m += loongson.o


all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

