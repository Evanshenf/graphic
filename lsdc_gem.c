/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_gem.c
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/

#include <linux/dma-buf.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>

#include "lsdc_drv.h"
#include "lsdc_gem.h"

void lsdc_gem_init(struct drm_device *ddev)
{
    struct drm_lsdc_private *ldev = to_lsdc(ddev);

    mutex_init(&ldev->gem.mutex);
    INIT_LIST_HEAD(&ldev->gem.objects);
}


