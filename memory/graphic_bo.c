// SPDX-License-Identifier: GPL-2.0
/*
 * Buffer ownership reorganized from graphic commit 18c1161, with the
 * scanout address checked while holding the BO reservation.
 * Retained code attribution:
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#include <linux/dma-resv.h>
#include <linux/fs.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <drm/drm_dumb_buffers.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_tt.h>

#include "graphic_device.h"
#include "memory/graphic_bo.h"

/* TTM state belongs to this layer and lives until the final DRM release. */
struct graphic_memory {
	struct ttm_device ttm;
	struct graphic_device *gdev;
};

static const struct ttm_place graphic_vram_place = {
	.mem_type = TTM_PL_VRAM,
};

static struct ttm_tt *graphic_tt_create(struct ttm_buffer_object *bo,
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

static int graphic_tt_populate(struct ttm_device *ttm, struct ttm_tt *tt,
			       struct ttm_operation_ctx *ctx)
{
	return ttm_pool_alloc(&ttm->pool, tt, ctx);
}

static void graphic_tt_unpopulate(struct ttm_device *ttm, struct ttm_tt *tt)
{
	ttm_pool_free(&ttm->pool, tt);
}

static void graphic_tt_destroy(struct ttm_device *ttm, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static void graphic_evict_flags(struct ttm_buffer_object *bo,
				struct ttm_placement *placement)
{
	static const struct ttm_place system = {
		.mem_type = TTM_PL_SYSTEM,
	};

	placement->placement = &system;
	placement->num_placement = 1;
}

static int graphic_move(struct ttm_buffer_object *bo, bool evict,
			struct ttm_operation_ctx *ctx,
			struct ttm_resource *new_mem, struct ttm_place *hop)
{
	if (!bo->resource) {
		ttm_bo_move_null(bo, new_mem);
		return 0;
	}
	return ttm_bo_move_memcpy(bo, ctx, new_mem);
}

static int graphic_io_mem_reserve(struct ttm_device *ttm,
				  struct ttm_resource *resource)
{
	struct graphic_memory *memory = container_of(ttm, struct graphic_memory, ttm);
	const struct graphic_vram *vram = &memory->gdev->vram;
	u64 offset, address;

	resource->bus.addr = NULL;
	resource->bus.offset = 0;
	resource->bus.is_iomem = false;
	resource->bus.caching = ttm_cached;
	if (resource->mem_type == TTM_PL_SYSTEM)
		return 0;
	if (resource->mem_type != TTM_PL_VRAM)
		return -EINVAL;
	if (resource->start > (U64_MAX >> PAGE_SHIFT))
		return -EOVERFLOW;

	offset = (u64)resource->start << PAGE_SHIFT;
	if (offset > vram->size || resource->size > vram->size - offset)
		return -EINVAL;
	if (check_add_overflow((u64)vram->cpu_start, offset, &address))
		return -EOVERFLOW;

	/* CPU mappings use the host resource, never the PCI scanout address. */
	resource->bus.offset = address;
	resource->bus.is_iomem = true;
	resource->bus.caching = ttm_write_combined;
	return 0;
}

static const struct ttm_device_funcs graphic_ttm_funcs = {
	.ttm_tt_create = graphic_tt_create,
	.ttm_tt_populate = graphic_tt_populate,
	.ttm_tt_unpopulate = graphic_tt_unpopulate,
	.ttm_tt_destroy = graphic_tt_destroy,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = graphic_evict_flags,
	.move = graphic_move,
	.io_mem_reserve = graphic_io_mem_reserve,
};

static void graphic_memory_fini(struct drm_device *drm, void *data)
{
	struct graphic_memory *memory = data;

	drm_WARN_ON(drm, ttm_range_man_fini(&memory->ttm, TTM_PL_VRAM));
	ttm_device_fini(&memory->ttm);
}

int graphic_memory_init(struct graphic_device *gdev)
{
	struct drm_device *drm = &gdev->drm;
	const struct graphic_vram *vram = &gdev->vram;
	u64 usable_size = round_down((u64)vram->size, PAGE_SIZE);
	struct graphic_memory *memory;
	u64 end;
	int ret;

	if (!usable_size || !IS_ALIGNED(vram->cpu_start, PAGE_SIZE) ||
	    !IS_ALIGNED(vram->bus_start, PAGE_SIZE))
		return -EINVAL;
	if (check_add_overflow((u64)vram->cpu_start, usable_size - 1, &end) ||
	    check_add_overflow(vram->bus_start, usable_size - 1, &end) ||
	    end > U32_MAX)
		return -ERANGE;

	memory = drmm_kzalloc(drm, sizeof(*memory), GFP_KERNEL);
	if (!memory)
		return -ENOMEM;
	memory->gdev = gdev;
	ret = ttm_device_init(&memory->ttm, &graphic_ttm_funcs, drm->dev,
			      drm->anon_inode->i_mapping, drm->vma_offset_manager, 0);
	if (ret)
		return ret;
	ret = ttm_range_man_init(&memory->ttm, TTM_PL_VRAM, false,
				 usable_size >> PAGE_SHIFT);
	if (ret) {
		ttm_device_fini(&memory->ttm);
		return ret;
	}
	ret = drmm_add_action_or_reset(drm, graphic_memory_fini, memory);
	if (ret)
		return ret;
	gdev->memory = memory;
	return 0;
}

static void graphic_bo_destroy(struct ttm_buffer_object *bo)
{
	drm_gem_object_release(&bo->base);
	kfree(bo);
}

static void graphic_gem_free(struct drm_gem_object *obj)
{
	ttm_bo_fini(drm_gem_ttm_of_gem(obj));
}

static int graphic_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(obj);
	int ret;

	dma_resv_assert_held(obj->resv);
	/* The mapping must remain stable after its caller releases this lock. */
	ttm_bo_pin(bo);
	ret = drm_gem_ttm_vmap(obj, map);
	if (ret)
		ttm_bo_unpin(bo);
	return ret;
}

static void graphic_gem_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	dma_resv_assert_held(obj->resv);
	if (iosys_map_is_null(map))
		return;
	drm_gem_ttm_vunmap(obj, map);
	ttm_bo_unpin(drm_gem_ttm_of_gem(obj));
}

static const struct drm_gem_object_funcs graphic_gem_funcs = {
	.free = graphic_gem_free,
	.print_info = drm_gem_ttm_print_info,
	.vmap = graphic_gem_vmap,
	.vunmap = graphic_gem_vunmap,
	.mmap = drm_gem_ttm_mmap,
};

static struct drm_gem_object *graphic_bo_create(struct graphic_device *gdev,
						size_t size)
{
	struct ttm_placement placement = {
		.num_placement = 1,
		.placement = &graphic_vram_place,
	};
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.gfp_retry_mayfail = true,
	};
	struct iosys_map map = { };
	struct ttm_buffer_object *bo;
	struct drm_gem_object *obj;
	int ret;

	if (!size || !IS_ALIGNED(size, PAGE_SIZE) || size > gdev->vram.size)
		return ERR_PTR(-EINVAL);
	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	obj = &bo->base;
	drm_gem_private_object_init(&gdev->drm, obj, size);
	obj->funcs = &graphic_gem_funcs;

	/* An initialization failure consumes bo and invokes graphic_bo_destroy. */
	ret = ttm_bo_init_reserved(&gdev->memory->ttm, bo, ttm_bo_type_device,
				   &placement, 1, &ctx, NULL, NULL, graphic_bo_destroy);
	if (ret)
		return ERR_PTR(ret);

	/* Hold the reservation until the full allocation, including padding, is zero. */
	ret = ttm_bo_vmap(bo, &map);
	if (ret)
		goto err_unreserve;
	iosys_map_memset(&map, 0, 0, size);
	/* Complete VRAM clearing before publishing a userspace handle. */
	wmb();
	ttm_bo_vunmap(bo, &map);
	ttm_bo_unreserve(bo);
	return obj;

err_unreserve:
	ttm_bo_unreserve(bo);
	drm_gem_object_put(obj);
	return ERR_PTR(ret);
}

int graphic_dumb_create(struct drm_file *file, struct drm_device *drm,
			struct drm_mode_create_dumb *args)
{
	struct graphic_device *gdev = to_graphic(drm);
	u64 usable_size = round_down((u64)gdev->vram.size, PAGE_SIZE);
	struct drm_gem_object *obj;
	int ret;

	if (args->bpp != 32)
		return -EINVAL;
	ret = drm_mode_size_dumb(drm, args, LS7A2000_STRIDE_ALIGN, 0);
	if (ret)
		return ret;
	if (args->size > usable_size || args->size > SIZE_MAX)
		return -ENOMEM;

	obj = graphic_bo_create(gdev, args->size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	ret = drm_gem_handle_create(file, obj, &args->handle);
	drm_gem_object_put(obj);
	return ret;
}

int graphic_bo_pin(struct drm_gem_object *obj, u64 *bus_address)
{
	struct ttm_placement placement = {
		.num_placement = 1,
		.placement = &graphic_vram_place,
	};
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.gfp_retry_mayfail = true,
	};
	const struct graphic_vram *vram;
	struct ttm_buffer_object *bo;
	struct ttm_resource *resource;
	u64 offset, address, end;
	int ret;

	if (!obj || !bus_address || obj->funcs != &graphic_gem_funcs)
		return -EINVAL;
	bo = drm_gem_ttm_of_gem(obj);
	vram = &to_graphic(obj->dev)->vram;
	ret = ttm_bo_reserve(bo, true, false, NULL);
	if (ret)
		return ret;
	ret = ttm_bo_validate(bo, &placement, &ctx);
	if (ret)
		goto out_unreserve;
	resource = bo->resource;
	if (!resource || resource->mem_type != TTM_PL_VRAM || !obj->size) {
		ret = -EINVAL;
		goto out_unreserve;
	}
	if (resource->start > (U64_MAX >> PAGE_SHIFT)) {
		ret = -EOVERFLOW;
		goto out_unreserve;
	}
	offset = (u64)resource->start << PAGE_SHIFT;
	if (offset > vram->size || obj->size > vram->size - offset) {
		ret = -EINVAL;
		goto out_unreserve;
	}
	if (check_add_overflow(vram->bus_start, offset, &address) ||
	    check_add_overflow(address, (u64)obj->size - 1, &end) ||
	    end > U32_MAX) {
		ret = -ERANGE;
		goto out_unreserve;
	}

	/* The complete BO fits; the returned PCI address stays valid while pinned. */
	ttm_bo_pin(bo);
	*bus_address = address;
	ret = 0;
out_unreserve:
	ttm_bo_unreserve(bo);
	return ret;
}

void graphic_bo_unpin(struct drm_gem_object *obj)
{
	struct ttm_buffer_object *bo;
	int ret;

	if (WARN_ON_ONCE(!obj || obj->funcs != &graphic_gem_funcs))
		return;
	bo = drm_gem_ttm_of_gem(obj);
	ret = ttm_bo_reserve(bo, false, false, NULL);
	if (WARN_ON_ONCE(ret))
		return;
	ttm_bo_unpin(bo);
	ttm_bo_unreserve(bo);
}
