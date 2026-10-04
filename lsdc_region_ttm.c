// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_tt.h>

#include "lsdc_drv.h"
#include "lsdc_region_ttm.h"

static struct ttm_tt *lsdc_ttm_tt_create(struct ttm_buffer_object *bo,
					 u32 page_flags)
{
	struct ttm_tt *tt;
	int ret;

	tt = kzalloc_obj(*tt);
	if (!tt)
		return NULL;

	ret = ttm_tt_init(tt, bo, page_flags | TTM_TT_FLAG_ZERO_ALLOC,
			  ttm_cached, 0);
	if (ret) {
		kfree(tt);
		return NULL;
	}

	return tt;
}

static int lsdc_ttm_tt_populate(struct ttm_device *bdev, struct ttm_tt *tt,
				struct ttm_operation_ctx *ctx)
{
	return ttm_pool_alloc(&bdev->pool, tt, ctx);
}

static void lsdc_ttm_tt_unpopulate(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_pool_free(&bdev->pool, tt);
}

static void lsdc_ttm_tt_destroy(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static void lsdc_ttm_evict_flags(struct ttm_buffer_object *bo,
				 struct ttm_placement *placement)
{
	static const struct ttm_place system = {
		.mem_type = TTM_PL_SYSTEM,
	};

	placement->placement = &system;
	placement->num_placement = 1;
}

static int lsdc_ttm_move(struct ttm_buffer_object *bo, bool evict,
			 struct ttm_operation_ctx *ctx,
			 struct ttm_resource *new_mem, struct ttm_place *hop)
{
	if (!bo->resource) {
		ttm_bo_move_null(bo, new_mem);
		return 0;
	}

	return ttm_bo_move_memcpy(bo, ctx, new_mem);
}

static int lsdc_ttm_io_mem_reserve(struct ttm_device *bdev,
				   struct ttm_resource *mem)
{
	struct drm_lsdc_private *dev_priv =
		container_of(bdev, struct drm_lsdc_private, bdev);
	u64 offset, address;

	mem->bus.addr = NULL;
	mem->bus.offset = 0;
	mem->bus.is_iomem = false;
	mem->bus.caching = ttm_cached;

	if (mem->mem_type == TTM_PL_SYSTEM)
		return 0;
	if (mem->mem_type != TTM_PL_VRAM)
		return -EINVAL;
	if (mem->start > (U64_MAX >> PAGE_SHIFT))
		return -EOVERFLOW;

	offset = (u64)mem->start << PAGE_SHIFT;
	if (offset > dev_priv->vram_size ||
	    mem->size > dev_priv->vram_size - offset)
		return -EINVAL;
	if (check_add_overflow((u64)dev_priv->vram_base, offset, &address))
		return -EOVERFLOW;

	mem->bus.offset = address;
	mem->bus.is_iomem = true;
	mem->bus.caching = ttm_write_combined;

	return 0;
}

static const struct ttm_device_funcs lsdc_ttm_funcs = {
	.ttm_tt_create = lsdc_ttm_tt_create,
	.ttm_tt_populate = lsdc_ttm_tt_populate,
	.ttm_tt_unpopulate = lsdc_ttm_tt_unpopulate,
	.ttm_tt_destroy = lsdc_ttm_tt_destroy,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = lsdc_ttm_evict_flags,
	.move = lsdc_ttm_move,
	.io_mem_reserve = lsdc_ttm_io_mem_reserve,
};

static void lsdc_ttm_fini(struct drm_device *drm, void *data)
{
	struct drm_lsdc_private *dev_priv = data;

	drm_WARN_ON(drm, ttm_range_man_fini(&dev_priv->bdev, TTM_PL_VRAM));
	ttm_device_fini(&dev_priv->bdev);
}

int lsdc_ttm_init(struct drm_lsdc_private *dev_priv)
{
	struct drm_device *drm = &dev_priv->drm_dev;
	u64 usable_size = round_down((u64)dev_priv->vram_size, PAGE_SIZE);
	u64 end;
	int ret;

	if (!usable_size || !IS_ALIGNED(dev_priv->vram_base, PAGE_SIZE) ||
	    !IS_ALIGNED(dev_priv->vram_bus_base, PAGE_SIZE))
		return -EINVAL;
	if (check_add_overflow((u64)dev_priv->vram_base, usable_size - 1, &end) ||
	    check_add_overflow(dev_priv->vram_bus_base, usable_size - 1, &end))
		return -EOVERFLOW;

	ret = ttm_device_init(&dev_priv->bdev, &lsdc_ttm_funcs, drm->dev,
			      drm->anon_inode->i_mapping, drm->vma_offset_manager, 0);
	if (ret)
		return ret;

	ret = ttm_range_man_init(&dev_priv->bdev, TTM_PL_VRAM, false,
				 usable_size >> PAGE_SHIFT);
	if (ret) {
		ttm_device_fini(&dev_priv->bdev);
		return ret;
	}

	return drmm_add_action_or_reset(drm, lsdc_ttm_fini, dev_priv);
}
