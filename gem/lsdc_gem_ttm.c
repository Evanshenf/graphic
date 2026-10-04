/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_gem_ttm.c
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/

#include <linux/shmem_fs.h>

#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/drm_buddy.h>
#include "lsdc_drv.h"
#include "lsdc_region_ttm.h"
#include "gem/lsdc_gem_ttm.h"




/*
 * All callbacks need to take care not to downcast a struct ttm_buffer_object
 * without checking its subclass, since it might be a TTM ghost object.
 */
static struct ttm_device_funcs lsdc_ttm_bo_driver = {
    .ttm_tt_create = NULL,
    .ttm_tt_populate = NULL,
    .ttm_tt_unpopulate = NULL,
    .ttm_tt_destroy = NULL,
    .eviction_valuable = NULL,
    .evict_flags = NULL,
    .swap_notify = NULL,
    .delete_mem_notify = NULL,
    .io_mem_pfn =  NULL,
    .access_memory = NULL,
};

/**
 * i915_ttm_driver - Return a pointer to the TTM device funcs
 *
 * Return: Pointer to statically allocated TTM device funcs.
 */
struct ttm_device_funcs *lsdc_ttm_driver(void)
{
    return &lsdc_ttm_bo_driver;
}


