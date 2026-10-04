// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <drm/drm_gem_ttm_helper.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>

#include "lsdc_drv.h"
#include "lsdc_gem.h"
#include "lsdc_gem_ttm.h"

struct lsdc_bo {
	struct ttm_buffer_object tbo;
	struct list_head link;
};

static const struct drm_gem_object_funcs lsdc_gem_object_funcs;

static const struct ttm_place lsdc_vram_place = {
	.mem_type = TTM_PL_VRAM,
};

static struct lsdc_bo *to_lsdc_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct lsdc_bo, tbo.base);
}

static int lsdc_bo_pin_vram(struct ttm_buffer_object *bo)
{
	struct ttm_placement placement = {
		.num_placement = 1,
		.placement = &lsdc_vram_place,
	};
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.gfp_retry_mayfail = true,
	};
	int ret;

	dma_resv_assert_held(bo->base.resv);

	ret = ttm_bo_validate(bo, &placement, &ctx);
	if (ret)
		return ret;

	ttm_bo_pin(bo);
	return 0;
}

static void lsdc_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct drm_lsdc_private *dev_priv = to_lsdc(tbo->base.dev);
	struct lsdc_bo *bo = container_of(tbo, struct lsdc_bo, tbo);

	mutex_lock(&dev_priv->gem.mutex);
	list_del_init(&bo->link);
	mutex_unlock(&dev_priv->gem.mutex);

	drm_gem_object_release(&tbo->base);
	kfree(bo);
}

static void lsdc_gem_free(struct drm_gem_object *obj)
{
	ttm_bo_fini(&to_lsdc_bo(obj)->tbo);
}

static int lsdc_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct ttm_buffer_object *bo = &to_lsdc_bo(obj)->tbo;
	int ret;

	dma_resv_assert_held(obj->resv);

	/* Keep the mapped resource stable after the caller drops this lock. */
	ttm_bo_pin(bo);
	ret = drm_gem_ttm_vmap(obj, map);
	if (ret)
		ttm_bo_unpin(bo);

	return ret;
}

static void lsdc_gem_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	dma_resv_assert_held(obj->resv);
	if (iosys_map_is_null(map))
		return;

	drm_gem_ttm_vunmap(obj, map);
	ttm_bo_unpin(&to_lsdc_bo(obj)->tbo);
}

static const struct drm_gem_object_funcs lsdc_gem_object_funcs = {
	.free = lsdc_gem_free,
	.print_info = drm_gem_ttm_print_info,
	.vmap = lsdc_gem_vmap,
	.vunmap = lsdc_gem_vunmap,
	.mmap = drm_gem_ttm_mmap,
};

struct drm_gem_object *lsdc_gem_ttm_create(struct drm_device *drm, size_t size)
{
	struct drm_lsdc_private *dev_priv = to_lsdc(drm);
	struct ttm_placement placement = {
		.num_placement = 1,
		.placement = &lsdc_vram_place,
	};
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.gfp_retry_mayfail = true,
	};
	struct iosys_map map = { };
	struct drm_gem_object *obj;
	struct lsdc_bo *bo;
	int ret;

	if (!size || !IS_ALIGNED(size, PAGE_SIZE) || size > dev_priv->vram_size)
		return ERR_PTR(-EINVAL);

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&bo->link);
	obj = &bo->tbo.base;
	drm_gem_private_object_init(drm, obj, size);
	obj->funcs = &lsdc_gem_object_funcs;

	/* Failure consumes the object and invokes lsdc_bo_destroy(). */
	ret = ttm_bo_init_reserved(&dev_priv->bdev, &bo->tbo, ttm_bo_type_device,
				   &placement, 1, &ctx, NULL, NULL, lsdc_bo_destroy);
	if (ret)
		return ERR_PTR(ret);

	/* The reservation stays held until all VRAM, including padding, is zero. */
	ret = ttm_bo_vmap(&bo->tbo, &map);
	if (ret)
		goto err_unreserve;

	iosys_map_memset(&map, 0, 0, size);
	/* Order VRAM initialization before users can submit it for scanout. */
	wmb();
	ttm_bo_vunmap(&bo->tbo, &map);

	mutex_lock(&dev_priv->gem.mutex);
	list_add_tail(&bo->link, &dev_priv->gem.objects);
	mutex_unlock(&dev_priv->gem.mutex);
	ttm_bo_unreserve(&bo->tbo);

	return obj;

err_unreserve:
	ttm_bo_unreserve(&bo->tbo);
	drm_gem_object_put(obj);
	return ERR_PTR(ret);
}

int lsdc_gem_pin(struct drm_gem_object *obj)
{
	struct ttm_buffer_object *bo;
	int ret;

	if (!obj || obj->funcs != &lsdc_gem_object_funcs)
		return -EINVAL;
	bo = &to_lsdc_bo(obj)->tbo;

	ret = ttm_bo_reserve(bo, true, false, NULL);
	if (ret)
		return ret;

	ret = lsdc_bo_pin_vram(bo);
	ttm_bo_unreserve(bo);

	return ret;
}

void lsdc_gem_unpin(struct drm_gem_object *obj)
{
	struct ttm_buffer_object *bo;
	int ret;

	if (WARN_ON_ONCE(!obj || obj->funcs != &lsdc_gem_object_funcs))
		return;
	bo = &to_lsdc_bo(obj)->tbo;

	ret = ttm_bo_reserve(bo, false, false, NULL);
	if (WARN_ON_ONCE(ret))
		return;

	ttm_bo_unpin(bo);
	ttm_bo_unreserve(bo);
}

int lsdc_gem_scanout_address(struct drm_gem_object *obj, u64 *address)
{
	struct drm_lsdc_private *dev_priv;
	struct ttm_buffer_object *bo;
	struct ttm_resource *resource;
	u64 offset;

	if (!obj || !address || obj->funcs != &lsdc_gem_object_funcs)
		return -EINVAL;
	bo = &to_lsdc_bo(obj)->tbo;
	resource = READ_ONCE(bo->resource);
	if (!READ_ONCE(bo->pin_count) || !resource ||
	    resource->mem_type != TTM_PL_VRAM)
		return -EINVAL;
	if (resource->start > (U64_MAX >> PAGE_SHIFT))
		return -EOVERFLOW;

	dev_priv = to_lsdc(obj->dev);
	offset = (u64)resource->start << PAGE_SHIFT;
	if (offset > dev_priv->vram_size ||
	    obj->size > dev_priv->vram_size - offset)
		return -EINVAL;
	if (check_add_overflow(dev_priv->vram_bus_base, offset, address))
		return -EOVERFLOW;

	return 0;
}
