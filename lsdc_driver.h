/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#ifndef LSDC_DRIVER_H
#define LSDC_DRIVER_H

struct pci_dev;
struct pci_device_id;

#define DRIVER_NAME "graphic"
#define DRIVER_DESC "Manual-based LS7A2000 display controller"

int lsdc_driver_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void lsdc_driver_remove(struct pci_dev *pdev);
void lsdc_driver_shutdown(struct pci_dev *pdev);

#endif
