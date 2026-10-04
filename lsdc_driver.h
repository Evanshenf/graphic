/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_driver.h
*   创 建 者：sw
*   创建日期：2024年05月13日
*   描    述：
*
================================================================*/


#ifndef LSDC_DRIVER_H__
#define LSDC_DRIVER_H__

#include <linux/pm.h>

struct pci_dev;
struct pci_device_id;


#define DRIVER_NAME "lsdc"
#define DRIVER_DESC "Loongson Graphics "
#define DRIVER_DATE "20240513"
#define DRIVER_TIMESTAMP  1715529600
int lsdc_driver_probe(struct pci_dev *pdev, const struct pci_device_id *ent);

#endif //LSDC_DRIVER_H__

