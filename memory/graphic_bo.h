/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GRAPHIC_BO_H
#define GRAPHIC_BO_H

#include <linux/types.h>

struct graphic_device;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_mode_create_dumb;

int graphic_memory_init(struct graphic_device *gdev);
int graphic_dumb_create(struct drm_file *file, struct drm_device *drm,
			struct drm_mode_create_dumb *args);
/* On success the caller owns one VRAM pin and receives its PCI bus address. */
int graphic_bo_pin(struct drm_gem_object *obj, u64 *bus_address);
void graphic_bo_unpin(struct drm_gem_object *obj);

#endif
