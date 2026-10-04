/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_gem.h
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/


#ifndef LSDC_GEM_H__
#define LSDC_GEM_H__
#include <drm/drm_device.h>
#include <drm/drm_gem.h>


void lsdc_gem_init(struct drm_device *ddev);

#endif //LSDC_GEM_H__

