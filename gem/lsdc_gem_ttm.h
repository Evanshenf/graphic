/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#ifndef LSDC_GEM_TTM_H
#define LSDC_GEM_TTM_H

#include <linux/types.h>

struct drm_device;
struct drm_gem_object;

struct drm_gem_object *lsdc_gem_ttm_create(struct drm_device *drm, size_t size);

#endif
