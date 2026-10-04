/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：intel_vga.c
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/


#include <linux/vgaarb.h>

#include <video/vga.h>

#include "lsdc_drv.h"
#include "soc/lsdc_gmch.h"
#include "lsdc_reg.h"
#include "lsdc_vga.h"


int lsdc_vga_register(struct drm_lsdc_private *lsdc)
{

    struct pci_dev *pdev = to_pci_dev(lsdc->drm_dev.dev);
    int ret;

    ret = vga_client_register(pdev,lsdc_gmch_vga_set_decode);
    if (ret && ret != -ENODEV)
        return ret;

    return 0;
}

void lsdc_vga_unregister(struct drm_lsdc_private *lsdc)
{
    struct pci_dev *pdev = to_pci_dev(lsdc->drm_dev.dev);

    vga_client_unregister(pdev);
}

