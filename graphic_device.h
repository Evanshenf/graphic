/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GRAPHIC_DEVICE_H
#define GRAPHIC_DEVICE_H

#include <linux/pci.h>
#include <drm/drm_device.h>

#include "hardware/ls7a2000.h"

struct graphic_memory;
struct graphic_kms;

struct graphic_vram {
	resource_size_t cpu_start;
	resource_size_t size;
	u64 bus_start;
};

/* Aggregates subsystem owners; their private objects stay in their own layer. */
struct graphic_device {
	struct drm_device drm;
	struct pci_dev *pci;
	struct graphic_vram vram;
	struct ls7a2000_hw hw;
	struct graphic_memory *memory;
	struct graphic_kms *kms;
};

static inline struct graphic_device *to_graphic(struct drm_device *drm)
{
	return container_of(drm, struct graphic_device, drm);
}

#endif
