/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：loongson_device.c
*   创 建 者：sw
*   创建日期：2024年05月18日
*   描    述：
*
================================================================*/


#include <linux/pci.h>

#include "lsdc_drv.h"

#include "lsdc_irq.h"



 const struct lsdc_kms_funcs ls7a2000_kms_funcs = {
    .create_i2c = lsdc_create_i2c_chan,
    .irq_handler = ls7a2000_dc_irq_handler,
    .output_init = ls7a2000_output_init,
    .cursor_plane_init = ls7a2000_cursor_plane_init,
    .primary_plane_init = lsdc_primary_plane_init,
    //.crtc_init = ls7a1000_crtc_init,
};

