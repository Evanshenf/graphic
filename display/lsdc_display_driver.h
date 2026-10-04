/*================================================================
 *   Copyright (C) 2024Ltd. All rights reserved.
 *   
 *   文件名称：lsdc_display_driver.h
 *   创 建 者：sw
 *   创建日期：2024年05月15日
 *   描    述：
 *
 ================================================================*/


#ifndef LSDC_DISPLAY_DRIVER_H__
#define LSDC_DISPLAY_DRIVER_H__
#include <linux/types.h>
#include "lsdc_drv.h"

int lsdc_display_driver_probe_noirq (struct drm_lsdc_private *lsdc);

void lsdc_display_driver_register(struct drm_lsdc_private *lsdc);

#endif //LSDC_DISPLAY_DRIVER_H__

