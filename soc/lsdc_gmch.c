/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：intel_gmch.c
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/

#include <linux/pci.h>
#include <linux/pnp.h>
#include <linux/vgaarb.h>

#include <drm/drm_managed.h>

#include "lsdc_drv.h"
#include "lsdc_gmch.h"

unsigned int lsdc_gmch_vga_set_decode(struct pci_dev *pdev, bool enable_decode)
{
    //struct drm_lsdc_private *lsdc = pdev_to_lsdc(pdev);
        return VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;
}

