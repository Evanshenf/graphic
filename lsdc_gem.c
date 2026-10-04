// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#include <linux/err.h>
#include <linux/mm.h>

#include <drm/drm_dumb_buffers.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode.h>

#include "gem/lsdc_gem_ttm.h"
#include "lsdc_drv.h"
#include "lsdc_gem.h"

void lsdc_gem_init(struct drm_device *drm)
{
	struct drm_lsdc_private *dev_priv = to_lsdc(drm);

	mutex_init(&dev_priv->gem.mutex);
	INIT_LIST_HEAD(&dev_priv->gem.objects);
}

int lsdc_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	struct drm_lsdc_private *dev_priv = to_lsdc(drm);
	struct drm_gem_object *obj;
	u64 usable_size = round_down((u64)dev_priv->vram_size, PAGE_SIZE);
	int ret;

	if (args->bpp != 32)
		return -EINVAL;

	ret = drm_mode_size_dumb(drm, args, LSDC_STRIDE_ALIGN, 0);
	if (ret)
		return ret;
	if (args->size > usable_size || args->size > SIZE_MAX)
		return -ENOMEM;

	obj = lsdc_gem_ttm_create(drm, args->size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, obj, &args->handle);
	drm_gem_object_put(obj);

	return ret;
}
