// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/interrupt.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "lsdc_drv.h"
#include "lsdc_regs.h"
#include "lsdc_display_driver.h"
#include "lsdc_output.h"

static u16 lsdc_vsync_mask(unsigned int index)
{
	return index ? LSDC_IRQ_VSYNC1 : LSDC_IRQ_VSYNC0;
}

static u32 lsdc_crtc_vblank_counter(struct drm_crtc *crtc)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);

	return lsdc_read(pipe->dev_priv,
			 LSDC_PIPE_REG(LSDC_VSYNC_COUNTER, pipe->index));
}

static int lsdc_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	dev_priv->irq_mask |= lsdc_vsync_mask(pipe->index);
	/* Clear only this pipe's stale status while enabling its interrupt. */
	lsdc_write(dev_priv, LSDC_INTERRUPT,
		   (u32)dev_priv->irq_mask << 16 | lsdc_vsync_mask(pipe->index));
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
	return 0;
}

static void lsdc_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	dev_priv->irq_mask &= ~lsdc_vsync_mask(pipe->index);
	lsdc_write(dev_priv, LSDC_INTERRUPT, (u32)dev_priv->irq_mask << 16);
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static int lsdc_crtc_check(struct drm_crtc *crtc,
			   struct drm_atomic_commit *commit)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_crtc_state *state;
	struct drm_plane_state *primary_state;
	struct drm_display_mode *mode;

	state = drm_atomic_get_new_crtc_state(commit, crtc);
	if (!state->enable)
		return 0;
	primary_state = drm_atomic_get_new_plane_state(commit, crtc->primary);
	if (!primary_state)
		primary_state = crtc->primary->state;
	if (!primary_state || primary_state->crtc != crtc ||
	    !primary_state->fb || !primary_state->visible)
		return -EINVAL;
	mode = &state->adjusted_mode;
	if (!pipe->output.boot_mode_valid ||
	    mode->clock != pipe->output.boot_mode.clock ||
	    !drm_mode_equal(mode, &pipe->output.boot_mode))
		return -EINVAL;
	return drm_atomic_helper_check_crtc_primary_plane(state);
}

static void lsdc_crtc_stop_scanout(struct drm_crtc *crtc)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	u32 reg = LSDC_PIPE_REG(LSDC_FB_CONFIG, pipe->index);

	lsdc_write(dev_priv, pipe->index ? LSDC_CURSOR_CONFIG1 :
		   LSDC_CURSOR_CONFIG0, 0);
	lsdc_write(dev_priv, reg, lsdc_read(dev_priv, reg) &
		   ~(LSDC_FB_ENABLE | LSDC_FB_SWITCH));
	lsdc_read(dev_priv, reg);
	/* No DMA-idle bit is documented. Allow a complete 60 Hz frame to drain. */
	usleep_range(20000, 21000);
	lsdc_output_disable(pipe);
	drm_crtc_vblank_off(crtc);
}

void lsdc_crtc_abort_pending(struct drm_crtc *crtc)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	event = pipe->event;
	if (!event) {
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		return;
	}
	/* The IRQ must not release the old framebuffer while scanout drains. */
	pipe->event = NULL;
	WRITE_ONCE(pipe->scanout_fault, true);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

	lsdc_crtc_stop_scanout(crtc);
	drm_err(crtc->dev, "CRTC%u flip timed out; scanout stopped\n",
		pipe->index);
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, event);
	drm_crtc_vblank_put(crtc);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void lsdc_crtc_queue_event(struct drm_crtc *crtc,
				  struct drm_crtc_state *state)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	int ret;

	if (!state->event)
		return;
	/* Dependencies normally finish the preceding flip before this point. */
	lsdc_crtc_abort_pending(crtc);
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (!state->active || READ_ONCE(pipe->scanout_fault)) {
		drm_crtc_send_vblank_event(crtc, state->event);
		state->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		return;
	}
	ret = drm_crtc_vblank_get(crtc);
	event = state->event;
	state->event = NULL;
	if (ret)
		WRITE_ONCE(pipe->scanout_fault, true);
	else
		pipe->event = event;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	if (!ret)
		return;

	/* Without a vblank reference, completion cannot prove DMA has stopped. */
	lsdc_crtc_stop_scanout(crtc);
	drm_err(crtc->dev, "CRTC%u vblank unavailable: %d; scanout stopped\n",
		pipe->index, ret);
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void lsdc_crtc_begin(struct drm_crtc *crtc,
			    struct drm_atomic_commit *commit)
{
	/* Do not overwrite the bank condition of an unfinished preceding flip. */
	lsdc_crtc_abort_pending(crtc);
	crtc_to_pipe(crtc)->event_requires_bank = false;
}

static void lsdc_crtc_flush(struct drm_crtc *crtc,
			    struct drm_atomic_commit *commit)
{
	struct drm_crtc_state *state;

	state = drm_atomic_get_new_crtc_state(commit, crtc);
	/* Modeset scanout and vblank are enabled after the planes are flushed. */
	if (state->active && drm_atomic_crtc_needs_modeset(state))
		return;
	lsdc_crtc_queue_event(crtc, state);
}

static void lsdc_crtc_enable(struct drm_crtc *crtc,
			     struct drm_atomic_commit *commit)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	struct drm_crtc_state *state;
	struct drm_display_mode *mode;
	u32 reg, sync, config;
	int ret;

	state = drm_atomic_get_new_crtc_state(commit, crtc);
	mode = &state->adjusted_mode;
	lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_HDISPLAY, pipe->index),
		   mode->htotal << 16 | mode->hdisplay);
	lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_VDISPLAY, pipe->index),
		   mode->vtotal << 16 | mode->vdisplay);
	reg = LSDC_PIPE_REG(LSDC_HSYNC, pipe->index);
	sync = lsdc_read(dev_priv, reg) & LSDC_SYNC_INVERT;
	lsdc_write(dev_priv, reg, sync | LSDC_SYNC_ENABLE |
		   mode->hsync_end << 16 | mode->hsync_start);
	reg = LSDC_PIPE_REG(LSDC_VSYNC, pipe->index);
	sync = lsdc_read(dev_priv, reg) & LSDC_SYNC_INVERT;
	lsdc_write(dev_priv, reg, sync | LSDC_SYNC_ENABLE |
		   mode->vsync_end << 16 | mode->vsync_start);
	/* Start with linear 32-bit pixels, no gamma or dithering. */
	lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_DITHER_CONFIG, pipe->index), 0);
	reg = LSDC_PIPE_REG(LSDC_FB_CONFIG, pipe->index);
	config = lsdc_read(dev_priv, reg);
	config &= ~(LSDC_FB_FORMAT | LSDC_FB_TILE | LSDC_FB_GAMMA |
		    LSDC_FB_DMA | LSDC_FB_CROSS_ROUTE | LSDC_FB_BANK |
		    LSDC_FB_SWITCH | LSDC_FB_ENABLE);
	config |= LSDC_FB_RESET | LSDC_FB_ARGB8888;
	lsdc_write(dev_priv, reg, config);
	ret = lsdc_output_enable(pipe, mode);
	if (ret) {
		drm_err(crtc->dev, "HDMI%u enable failed: %d\n", pipe->index, ret);
		WRITE_ONCE(pipe->scanout_fault, true);
		lsdc_crtc_stop_scanout(crtc);
		lsdc_crtc_queue_event(crtc, state);
		return;
	}
	/* Publish framebuffer contents and timings before enabling scanout. */
	wmb();
	lsdc_write(dev_priv, reg, config | LSDC_FB_ENABLE);
	drm_crtc_vblank_on(crtc);
	WRITE_ONCE(pipe->scanout_fault, false);
	lsdc_crtc_queue_event(crtc, state);
}

static void lsdc_crtc_disable(struct drm_crtc *crtc,
			      struct drm_atomic_commit *commit)
{
	struct lsdc_display_pipe *pipe = crtc_to_pipe(crtc);
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	event = pipe->event;
	pipe->event = NULL;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	lsdc_crtc_stop_scanout(crtc);
	if (event) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, event);
		drm_crtc_vblank_put(crtc);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static const struct drm_crtc_funcs lsdc_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.get_vblank_counter = lsdc_crtc_vblank_counter,
	.enable_vblank = lsdc_crtc_enable_vblank,
	.disable_vblank = lsdc_crtc_disable_vblank,
};

static const struct drm_crtc_helper_funcs lsdc_crtc_helpers = {
	.atomic_check = lsdc_crtc_check,
	.atomic_begin = lsdc_crtc_begin,
	.atomic_flush = lsdc_crtc_flush,
	.atomic_enable = lsdc_crtc_enable,
	.atomic_disable = lsdc_crtc_disable,
};

int lsdc_crtc_init(struct lsdc_display_pipe *pipe)
{
	struct drm_device *drm = &pipe->dev_priv->drm_dev;
	int ret;

	pipe->crtc.pipe = pipe;
	ret = drm_crtc_init_with_planes(drm, &pipe->crtc.base,
					&pipe->primary.base, &pipe->cursor.base,
					&lsdc_crtc_funcs, "lsdc-crtc-%u",
					pipe->index);
	if (ret)
		return ret;
	drm_crtc_helper_add(&pipe->crtc.base, &lsdc_crtc_helpers);
	drm->max_vblank_count = U32_MAX;
	return 0;
}

static irqreturn_t lsdc_irq_handler(int irq, void *data)
{
	struct drm_lsdc_private *dev_priv = data;
	struct drm_device *drm = &dev_priv->drm_dev;
	unsigned long flags;
	u16 status;
	unsigned int i;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	status = lsdc_read(dev_priv, LSDC_INTERRUPT) & dev_priv->irq_mask;
	if (status)
		lsdc_write(dev_priv, LSDC_INTERRUPT,
			   (u32)dev_priv->irq_mask << 16 | status);
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
	if (!status)
		return IRQ_NONE;

	for (i = 0; i < LSDC_NUM_PIPES; i++) {
		struct lsdc_display_pipe *pipe = &dev_priv->dispipe[i];
		struct drm_crtc *crtc = &pipe->crtc.base;
		bool bank;

		if (!(status & lsdc_vsync_mask(i)))
			continue;
		drm_crtc_handle_vblank(crtc);
		spin_lock_irqsave(&drm->event_lock, flags);
		bank = !!(lsdc_read(dev_priv, LSDC_PIPE_REG(LSDC_FB_CONFIG, i)) &
			  LSDC_FB_BANK);
		if (pipe->event && (!pipe->event_requires_bank ||
				    bank == pipe->next_bank)) {
			drm_crtc_send_vblank_event(crtc, pipe->event);
			pipe->event = NULL;
			drm_crtc_vblank_put(crtc);
		}
		spin_unlock_irqrestore(&drm->event_lock, flags);
	}
	return IRQ_HANDLED;
}

int lsdc_irq_init(struct drm_lsdc_private *dev_priv)
{
	spin_lock_init(&dev_priv->irq_lock);
	lsdc_write(dev_priv, LSDC_INTERRUPT, U16_MAX);
	return devm_request_irq(&dev_priv->pdev->dev, dev_priv->pdev->irq,
				lsdc_irq_handler, IRQF_SHARED, "graphic", dev_priv);
}

void lsdc_irq_fini(struct drm_lsdc_private *dev_priv)
{
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	dev_priv->irq_mask = 0;
	lsdc_write(dev_priv, LSDC_INTERRUPT, U16_MAX);
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
	synchronize_irq(dev_priv->pdev->irq);
}
