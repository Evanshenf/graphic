/*================================================================
*   Copyright (C) 2024Ltd. All rights reserved.
*   
*   文件名称：lsdc_region_ttm.h
*   创 建 者：sw
*   创建日期：2024年05月15日
*   描    述：
*
================================================================*/


#ifndef LSDC_REGION_TTM_H__
#define LSDC_REGION_TTM_H__


#include <linux/container_of.h>
#include <linux/iosys-map.h>
#include <linux/list.h>

#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_tt.h>
#include "gem/lsdc_gem_ttm.h"


#define LSDC_GEM_DOMAIN_SYSTEM          0x1
#define LSDC_GEM_DOMAIN_GTT             0x2
#define LSDC_GEM_DOMAIN_VRAM            0x4

struct lsdc_bo {
    struct ttm_buffer_object tbo;

    /* Protected by gem.mutex */
    struct list_head list;

    struct iosys_map map;

    unsigned int vmap_count;
    /* cross device driver sharing reference count */
    unsigned int sharing_count;

    struct ttm_bo_kmap_obj kmap;
    void *kptr;
    bool is_iomem;

    size_t size;

    u32 initial_domain;

    struct ttm_placement placement;
    struct ttm_place placements[4];
};

static inline struct ttm_buffer_object *to_ttm_bo(struct drm_gem_object *gem)
{
    return container_of(gem, struct ttm_buffer_object, base);
}

static inline struct lsdc_bo *to_lsdc_bo(struct ttm_buffer_object *tbo)
{
    return container_of(tbo, struct lsdc_bo, tbo);
}

static inline struct lsdc_bo *gem_to_lsdc_bo(struct drm_gem_object *gem)
{
    return container_of(gem, struct lsdc_bo, tbo.base);
}


size_t lsdc_bo_size(struct lsdc_bo *lbo);
void lsdc_bo_unreserve(struct lsdc_bo *lbo);
int lsdc_bo_reserve(struct lsdc_bo *lbo);

int lsdc_ttm_init(struct drm_lsdc_private *lsdc);
struct ttm_device_funcs *lsdc_ttm_driver(void);

void lsdc_bo_unpin(struct lsdc_bo *lbo);

void lsdc_bo_ref(struct lsdc_bo *lbo);
void lsdc_bo_unref(struct lsdc_bo *lbo);
u64 lsdc_bo_gpu_offset(struct lsdc_bo *lbo);
//static void lsdc_bo_set_placement(struct lsdc_bo *lbo, u32 domain);
#endif //LSDC_REGION_TTM_H__

