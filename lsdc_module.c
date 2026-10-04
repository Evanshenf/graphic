// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>

#include <drm/drm_drv.h>

#include "lsdc_driver.h"
#include "lsdc_pci.h"

static int __init lsdc_init(void)
{
	if (drm_firmware_drivers_only())
		return -ENODEV;

	return lsdc_pci_register_driver();
}

static void __exit lsdc_exit(void)
{
	lsdc_pci_unregister_driver();
}

module_init(lsdc_init);
module_exit(lsdc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("archwse");
MODULE_DESCRIPTION(DRIVER_DESC);
