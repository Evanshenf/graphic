// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */

#include <linux/module.h>
#include <linux/pci.h>

#include "lsdc_driver.h"
#include "lsdc_pci.h"

/* LS7A2000 user manual V1.3, section 20.1, D6:F1. */
static const struct pci_device_id lsdc_pci_ids[] = {
	{ PCI_DEVICE(0x0014, 0x7a36) },
	{ }
};
MODULE_DEVICE_TABLE(pci, lsdc_pci_ids);

static struct pci_driver lsdc_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = lsdc_pci_ids,
	.probe = lsdc_driver_probe,
	.remove = lsdc_driver_remove,
	.shutdown = lsdc_driver_shutdown,
};

int lsdc_pci_register_driver(void)
{
	return pci_register_driver(&lsdc_pci_driver);
}

void lsdc_pci_unregister_driver(void)
{
	pci_unregister_driver(&lsdc_pci_driver);
}
