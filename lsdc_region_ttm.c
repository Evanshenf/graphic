/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_region_ttm.c
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/

#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_range_manager.h>
#include "lsdc_drv.h"
#include "lsdc_region_ttm.h"
#include "gem/lsdc_gem_ttm.h"



static void lsdc_bo_set_placement(struct lsdc_bo *lbo, u32 domain);









u64 lsdc_bo_gpu_offset(struct lsdc_bo *lbo)
{
    struct ttm_buffer_object *tbo = &lbo->tbo;
    struct drm_device *ddev = tbo->base.dev;
    struct ttm_resource *resource = tbo->resource;

    if (unlikely(!tbo->pin_count)) {
        drm_err(ddev, "unpinned bo, gpu virtual address is invalid\n");
        return 0;
    }

    if (unlikely(resource->mem_type == TTM_PL_SYSTEM))
        return 0;

    return resource->start << PAGE_SHIFT;
}

int lsdc_ttm_init(struct drm_lsdc_private *lsdc)
{
	struct drm_device *drm= &lsdc->drm_dev;
	return ttm_device_init(&lsdc->bdev,lsdc_ttm_driver(),
			drm->dev, drm->anon_inode->i_mapping,
			drm->vma_offset_manager, false, true);
}
void lsdc_bo_unpin(struct lsdc_bo *lbo)
{
    struct ttm_buffer_object *tbo = &lbo->tbo;
    struct drm_lsdc_private *ldev = tdev_to_ldev(tbo->bdev);

    if (unlikely(!tbo->pin_count)) {
        drm_dbg(&ldev->drm_dev, "%p unpin is not necessary\n", lbo);
        return;
    }

    ttm_bo_unpin(tbo);

    if (!tbo->pin_count) {
        if (tbo->resource->mem_type == TTM_PL_VRAM)
            ldev->vram_pinned_size -= lsdc_bo_size(lbo);
        else if (tbo->resource->mem_type == TTM_PL_TT)
            ldev->gtt_pinned_size -= lsdc_bo_size(lbo);
    }
}




void lsdc_bo_ref(struct lsdc_bo *lbo)
{
    struct ttm_buffer_object *tbo = &lbo->tbo;

    ttm_bo_get(tbo);
}

void lsdc_bo_unref(struct lsdc_bo *lbo)
{
    struct ttm_buffer_object *tbo = &lbo->tbo;

    ttm_bo_put(tbo);
}



size_t lsdc_bo_size(struct lsdc_bo *lbo)
{
    struct ttm_buffer_object *tbo = &lbo->tbo;

    return tbo->base.size;
}

int lsdc_bo_reserve(struct lsdc_bo *lbo)
{
    return ttm_bo_reserve(&lbo->tbo, true, false, NULL);
}

void lsdc_bo_unreserve(struct lsdc_bo *lbo)
{
    return ttm_bo_unreserve(&lbo->tbo);
}

int lsdc_bo_pin(struct lsdc_bo *lbo, u32 domain, u64 *gpu_addr)
{
    struct ttm_operation_ctx ctx = { false, false };
    struct ttm_buffer_object *tbo = &lbo->tbo;
    struct drm_lsdc_private *ldev = tdev_to_ldev(tbo->bdev);
    int ret;

    if (tbo->pin_count)
        goto bo_pinned;

    if (lbo->sharing_count && domain == LSDC_GEM_DOMAIN_VRAM)
        return -EINVAL;

    if (domain)
        lsdc_bo_set_placement(lbo, domain);

    ret = ttm_bo_validate(tbo, &lbo->placement, &ctx);
    if (unlikely(ret)) {
        drm_err(&ldev->drm_dev, "%p validate failed: %d\n", lbo, ret);
        return ret;
    }

    if (domain == LSDC_GEM_DOMAIN_VRAM)
        ldev->vram_pinned_size += lsdc_bo_size(lbo);
    else if (domain == LSDC_GEM_DOMAIN_GTT)
        ldev->gtt_pinned_size += lsdc_bo_size(lbo);

bo_pinned:
    ttm_bo_pin(tbo);

    if (gpu_addr)
        *gpu_addr = lsdc_bo_gpu_offset(lbo);

    return 0;
}

static void lsdc_bo_set_placement(struct lsdc_bo *lbo, u32 domain)
{
    u32 c = 0;
    u32 pflags = 0;
    u32 i;

    if (lbo->tbo.base.size <= PAGE_SIZE)
        pflags |= TTM_PL_FLAG_TOPDOWN;

    lbo->placement.placement = lbo->placements;
    lbo->placement.busy_placement = lbo->placements;

    if (domain & LSDC_GEM_DOMAIN_VRAM) {
        lbo->placements[c].mem_type = TTM_PL_VRAM;
        lbo->placements[c++].flags = pflags;
    }

    if (domain & LSDC_GEM_DOMAIN_GTT) {
        lbo->placements[c].mem_type = TTM_PL_TT;
        lbo->placements[c++].flags = pflags;
    }

    if (domain & LSDC_GEM_DOMAIN_SYSTEM) {
        lbo->placements[c].mem_type = TTM_PL_SYSTEM;
        lbo->placements[c++].flags = 0;
    }

    if (!c) {
        lbo->placements[c].mem_type = TTM_PL_SYSTEM;
        lbo->placements[c++].flags = 0;
    }

    lbo->placement.num_placement = c;
    lbo->placement.num_busy_placement = c;

    for (i = 0; i < c; ++i) {
        lbo->placements[i].fpfn = 0;
        lbo->placements[i].lpfn = 0;
    }
}


