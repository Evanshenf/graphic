/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LSDC_DRV_H
#define LSDC_DRV_H

#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>
#include <drm/ttm/ttm_device.h>

#define LSDC_NUM_PIPES 2
#define LSDC_STRIDE_ALIGN 256
#define LSDC_MAX_WIDTH 1920
#define LSDC_MAX_HEIGHT 1080

struct drm_lsdc_private;
struct lsdc_display_pipe;

struct lsdc_gem {
	/* Protects the private GEM object list. */
	struct mutex mutex;
	struct list_head objects;
};

struct lsdc_crtc {
	struct drm_crtc base;
	struct lsdc_display_pipe *pipe;
};

struct lsdc_primary {
	struct drm_plane base;
	struct lsdc_display_pipe *pipe;
};

struct lsdc_cursor {
	struct drm_plane base;
	struct lsdc_display_pipe *pipe;
};

struct lsdc_output {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode boot_mode;
	u32 panel_config;
	u32 hdmi_control;
	u32 phy_control;
	u32 phy_pll;
	bool boot_mode_valid;
};

struct lsdc_display_pipe {
	struct drm_lsdc_private *dev_priv;
	struct lsdc_crtc crtc;
	struct lsdc_primary primary;
	struct lsdc_cursor cursor;
	struct lsdc_output output;
	unsigned int index;
	struct drm_pending_vblank_event *event;
	bool event_requires_bank;
	bool next_bank;
	bool scanout_fault;
};

struct drm_lsdc_private {
	struct drm_device drm_dev;
	struct pci_dev *pdev;
	void __iomem *ioregs;
	void __iomem *pll_regs;
	resource_size_t vram_base;
	resource_size_t vram_size;
	u64 vram_bus_base;
	struct ttm_device bdev;
	struct lsdc_gem gem;
	struct lsdc_display_pipe dispipe[LSDC_NUM_PIPES];
	/* Protects irq_mask and the mixed enable/W1C interrupt register. */
	spinlock_t irq_lock;
	u16 irq_mask;
};

static inline struct drm_lsdc_private *to_lsdc(struct drm_device *drm)
{
	return container_of(drm, struct drm_lsdc_private, drm_dev);
}

static inline struct lsdc_display_pipe *crtc_to_pipe(struct drm_crtc *crtc)
{
	return container_of(crtc, struct lsdc_crtc, base)->pipe;
}

static inline struct lsdc_display_pipe *primary_to_pipe(struct drm_plane *plane)
{
	return container_of(plane, struct lsdc_primary, base)->pipe;
}

static inline struct lsdc_display_pipe *cursor_to_pipe(struct drm_plane *plane)
{
	return container_of(plane, struct lsdc_cursor, base)->pipe;
}

static inline struct lsdc_display_pipe *output_to_pipe(struct lsdc_output *output)
{
	return container_of(output, struct lsdc_display_pipe, output);
}

static inline u32 lsdc_read(struct drm_lsdc_private *dev_priv, u32 reg)
{
	return readl(dev_priv->ioregs + reg);
}

static inline void lsdc_write(struct drm_lsdc_private *dev_priv, u32 reg, u32 value)
{
	writel(value, dev_priv->ioregs + reg);
}

#endif
