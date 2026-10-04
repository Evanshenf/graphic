/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_pci.h
*   创 建 者：sw
*   创建日期：2024年05月13日
*   描    述：
*
================================================================*/


#ifndef LSDC_PCI_H__
#define LSDC_PCI_H__

#include <linux/types.h>

struct pci_dev;

int lsdc_pci_register_driver(void);
void lsdc_pci_unregister_driver(void);



#endif //LSDC_PCI_H__

