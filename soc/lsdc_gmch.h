/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：intel_gmch.h
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/


#ifndef INTEL_GMCH_H__
#define INTEL_GMCH_H__

#include <linux/types.h>

struct pci_dev;
struct drm_lsdc_private;

unsigned int lsdc_gmch_vga_set_decode(struct pci_dev *pdev, bool enable_decode);


#endif //INTEL_GMCH_H__

