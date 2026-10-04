/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#ifndef LSDC_GEM_H
#define LSDC_GEM_H

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_mode_create_dumb;

void lsdc_gem_init(struct drm_device *drm);
int lsdc_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args);

/* These acquire the BO reservation lock and may sleep. */
int lsdc_gem_pin(struct drm_gem_object *obj);
void lsdc_gem_unpin(struct drm_gem_object *obj);

/* The caller must retain an object reference and a successful scanout pin. */
int lsdc_gem_scanout_address(struct drm_gem_object *obj, u64 *address);

#endif
