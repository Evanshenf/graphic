// SPDX-License-Identifier: GPL-2.0
#include <linux/overflow.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>

#include "lsdc_drv.h"
#include "lsdc_gem.h"
#include "lsdc_regs.h"
#include "lsdc_display_driver.h"

static const u32 lsdc_cursor_config[] = {
	LSDC_CURSOR_CONFIG0, LSDC_CURSOR_CONFIG1,
};

static const u32 lsdc_cursor_address[] = {
	LSDC_CURSOR_ADDRESS0, LSDC_CURSOR_ADDRESS1,
};

static const u32 lsdc_cursor_high[] = {
	LSDC_CURSOR_HIGH0, LSDC_CURSOR_HIGH1,
};

static const u32 lsdc_cursor_position[] = {
	LSDC_CURSOR_POSITION0, LSDC_CURSOR_POSITION1,
};

static int lsdc_plane_prepare_fb(struct drm_plane *plane,
				 struct drm_plane_state *state)
{
	struct drm_gem_object *obj;
	int ret;

	if (!state->fb)
		return 0;
	obj = drm_gem_fb_get_obj(state->fb, 0);
	ret = lsdc_gem_pin(obj);
	if (ret)
		return ret;
	ret = drm_gem_plane_helper_prepare_fb(plane, state);
	if (ret)
		lsdc_gem_unpin(obj);
	return ret;
}

static void lsdc_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	if (state->fb)
		lsdc_gem_unpin(drm_gem_fb_get_obj(state->fb, 0));
}

static int lsdc_plane_check(struct drm_plane *plane,
			    struct drm_atomic_commit *commit)
{
	struct drm_plane_state *state;
	struct drm_crtc_state *crtc_state;
	struct drm_gem_object *obj;
	u64 last, row;
	bool cursor = plane->type == DRM_PLANE_TYPE_CURSOR;
	int ret;

	state = drm_atomic_get_new_plane_state(commit, plane);
	if (!state->crtc || !state->fb)
		return 0;
	crtc_state = drm_atomic_get_crtc_state(commit, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  cursor, false);
	if (ret)
		return ret;
	if (cursor && !state->visible)
		return 0;
	if (state->src_x & 0xffff || state->src_y & 0xffff ||
	    state->src_w & 0xffff || state->src_h & 0xffff ||
	    state->fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	if (state->fb->pitches[0] % LSDC_STRIDE_ALIGN)
		return -EINVAL;

	obj = drm_gem_fb_get_obj(state->fb, 0);
	row = (u64)(state->src_y >> 16) + (state->src_h >> 16);
	if (!row)
		return -EINVAL;
	last = state->fb->offsets[0] +
		(row - 1) * state->fb->pitches[0] +
		((u64)(state->src_x >> 16) + (state->src_w >> 16)) * 4;
	if (last > obj->size)
		return -EINVAL;

	if (cursor) {
		/* No cursor stride register is documented: use packed 64x64. */
		if (state->fb->width != 64 || state->fb->height != 64 ||
		    state->fb->pitches[0] != 256 || state->src_x || state->src_y ||
		    state->src_w != 64 << 16 || state->src_h != 64 << 16 ||
		    state->crtc_x < -63 || state->crtc_y < -63 ||
		    state->crtc_x > 4095 || state->crtc_y > 4095)
			return -EINVAL;
	} else {
		if (state->crtc_x || state->crtc_y ||
		    state->crtc_w != crtc_state->mode.hdisplay ||
		    state->crtc_h != crtc_state->mode.vdisplay)
			return -EINVAL;
	}
	return 0;
}

static int lsdc_plane_address(struct drm_plane_state *state, u64 *address)
{
	struct drm_gem_object *obj = drm_gem_fb_get_obj(state->fb, 0);
	u64 offset;
	int ret;

	ret = lsdc_gem_scanout_address(obj, address);
	if (ret)
		return ret;
	offset = state->fb->offsets[0] +
		 (u64)(state->src_y >> 16) * state->fb->pitches[0] +
		 (u64)(state->src_x >> 16) * 4;
	return check_add_overflow(*address, offset, address) ||
	       *address > U32_MAX ? -ERANGE : 0;
}

static void lsdc_primary_update(struct drm_plane *plane,
				struct drm_atomic_commit *commit)
{
	struct lsdc_display_pipe *pipe = primary_to_pipe(plane);
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	struct drm_plane_state *state;
	struct drm_crtc_state *crtc_state;
	u32 config, reg;
	u64 address;

	state = drm_atomic_get_new_plane_state(commit, plane);
	if (!state->fb || !state->visible)
		return;
	if (WARN_ON(lsdc_plane_address(state, &address)))
		return;
	crtc_state = drm_atomic_get_new_crtc_state(commit, state->crtc);
	reg = LSDC_PIPE_REG(LSDC_FB_CONFIG, pipe->index);
	config = lsdc_read(dev_priv, reg);
	if (drm_atomic_crtc_needs_modeset(crtc_state) ||
	    !(config & LSDC_FB_ENABLE)) {
		lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_FB_ADDRESS0, pipe->index),
			   lower_32_bits(address));
		lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_FB_ADDRESS1, pipe->index),
			   lower_32_bits(address));
		lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_FB_STRIDE, pipe->index),
			   state->fb->pitches[0]);
		lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_FB_ORIGIN, pipe->index), 0);
		pipe->event_requires_bank = false;
	} else {
		pipe->next_bank = !(config & LSDC_FB_BANK);
		reg = pipe->next_bank ? LSDC_FB_ADDRESS1 : LSDC_FB_ADDRESS0;
		lsdc_write(dev_priv, LSDC_PIPE_REG(reg, pipe->index),
			   lower_32_bits(address));
		/* Publish the new VRAM contents and address before switching banks. */
		wmb();
		lsdc_write(dev_priv, LSDC_PIPE_REG(LSDC_FB_CONFIG, pipe->index),
			   (config & ~LSDC_FB_BANK) | LSDC_FB_SWITCH);
		pipe->event_requires_bank = true;
	}
}

static void lsdc_cursor_update(struct drm_plane *plane,
			       struct drm_atomic_commit *commit)
{
	struct lsdc_display_pipe *pipe = cursor_to_pipe(plane);
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	struct drm_plane_state *state;
	u32 config, position;
	u64 address;
	int x, y;

	state = drm_atomic_get_new_plane_state(commit, plane);
	if (!state->fb || !state->visible) {
		lsdc_write(dev_priv, lsdc_cursor_config[pipe->index], 0);
		return;
	}
	if (WARN_ON(lsdc_plane_address(state, &address)))
		return;
	x = state->crtc_x;
	y = state->crtc_y;
	/* The location is the hot spot; clipping needs hardware validation. */
	config = LSDC_CURSOR_ARGB8888 | LSDC_CURSOR_64 |
		 (pipe->index ? LSDC_CURSOR_ROUTE : 0) |
		 ((u32)max(-x, 0) << 16) | ((u32)max(-y, 0) << 8);
	position = ((u32)max(y, 0) << 16) | (u32)max(x, 0);
	lsdc_write(dev_priv, lsdc_cursor_config[pipe->index], 0);
	lsdc_write(dev_priv, lsdc_cursor_high[pipe->index],
		   upper_32_bits(address));
	lsdc_write(dev_priv, lsdc_cursor_address[pipe->index],
		   lower_32_bits(address));
	lsdc_write(dev_priv, lsdc_cursor_position[pipe->index], position);
	/* Publish VRAM contents and pointer registers before enabling the cursor. */
	wmb();
	lsdc_write(dev_priv, lsdc_cursor_config[pipe->index], config);
}

static void lsdc_cursor_disable(struct drm_plane *plane,
				struct drm_atomic_commit *commit)
{
	struct lsdc_display_pipe *pipe = cursor_to_pipe(plane);

	lsdc_write(pipe->dev_priv, lsdc_cursor_config[pipe->index], 0);
}

static const struct drm_plane_funcs lsdc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs lsdc_primary_helpers = {
	.prepare_fb = lsdc_plane_prepare_fb,
	.cleanup_fb = lsdc_plane_cleanup_fb,
	.atomic_check = lsdc_plane_check,
	.atomic_update = lsdc_primary_update,
};

static const struct drm_plane_helper_funcs lsdc_cursor_helpers = {
	.prepare_fb = lsdc_plane_prepare_fb,
	.cleanup_fb = lsdc_plane_cleanup_fb,
	.atomic_check = lsdc_plane_check,
	.atomic_update = lsdc_cursor_update,
	.atomic_disable = lsdc_cursor_disable,
};

int lsdc_plane_init(struct lsdc_display_pipe *pipe)
{
	static const u32 primary_formats[] = { DRM_FORMAT_XRGB8888 };
	static const u32 cursor_formats[] = { DRM_FORMAT_ARGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR,
					DRM_FORMAT_MOD_INVALID };
	struct drm_device *drm = &pipe->dev_priv->drm_dev;
	int ret;

	pipe->primary.pipe = pipe;
	pipe->cursor.pipe = pipe;
	ret = drm_universal_plane_init(drm, &pipe->primary.base, BIT(pipe->index),
				       &lsdc_plane_funcs, primary_formats,
				       ARRAY_SIZE(primary_formats), modifiers,
				       DRM_PLANE_TYPE_PRIMARY, "lsdc-primary-%u",
				       pipe->index);
	if (ret)
		return ret;
	drm_plane_helper_add(&pipe->primary.base, &lsdc_primary_helpers);
	ret = drm_universal_plane_init(drm, &pipe->cursor.base, BIT(pipe->index),
				       &lsdc_plane_funcs, cursor_formats,
				       ARRAY_SIZE(cursor_formats), modifiers,
				       DRM_PLANE_TYPE_CURSOR, "lsdc-cursor-%u",
				       pipe->index);
	if (ret)
		return ret;
	drm_plane_helper_add(&pipe->cursor.base, &lsdc_cursor_helpers);
	return drm_plane_create_blend_mode_property(&pipe->cursor.base,
						  BIT(DRM_MODE_BLEND_COVERAGE));
}
