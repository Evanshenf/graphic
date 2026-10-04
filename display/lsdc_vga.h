/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：intel_vga.h
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/


#ifndef INTEL_VGA_H__
#define INTEL_VGA_H__

struct drm_lsdc_private;
int lsdc_vga_register(struct drm_lsdc_private *lsdc);

void lsdc_vga_unregister(struct drm_lsdc_private *lsdc);
#endif //INTEL_VGA_H__

