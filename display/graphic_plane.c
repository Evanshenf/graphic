// SPDX-License-Identifier: GPL-2.0
/* DRM plane validation and one scanout pin per prepared plane state. */
#include <linux/overflow.h>
#include <linux/slab.h>

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

#include "graphic_device.h"
#include "memory/graphic_bo.h"
#include "graphic_display.h"

struct graphic_plane_state {
	struct drm_plane_state base;
	/* Checked byte interval in the BO, with an exclusive end. */
	u64 offset;
	u64 end;
	/* Stable from successful prepare until this state's cleanup. */
	u32 bus_address;
	bool owns_pin;
};

static struct graphic_plane_state *
graphic_plane_state(struct drm_plane_state *state)
{
	return container_of(state, struct graphic_plane_state, base);
}

static void graphic_plane_cleanup_fb(struct drm_plane *plane,
				     struct drm_plane_state *state)
{
	struct graphic_plane_state *gstate = graphic_plane_state(state);

	if (!gstate->owns_pin)
		return;
	graphic_bo_unpin(drm_gem_fb_get_obj(state->fb, 0));
	gstate->owns_pin = false;
}

static void graphic_plane_destroy_state(struct drm_plane *plane,
					struct drm_plane_state *state)
{
	/* Also handles a final prepared state during mode-config destruction. */
	graphic_plane_cleanup_fb(plane, state);
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(graphic_plane_state(state));
}

static void graphic_plane_reset(struct drm_plane *plane)
{
	struct graphic_plane_state *state;

	if (plane->state)
		graphic_plane_destroy_state(plane, plane->state);
	state = kzalloc_obj(*state);
	__drm_atomic_helper_plane_reset(plane, state ? &state->base : NULL);
}

static struct drm_plane_state *
graphic_plane_duplicate_state(struct drm_plane *plane)
{
	struct graphic_plane_state *state;

	if (!plane->state)
		return NULL;
	state = kzalloc_obj(*state);
	if (!state)
		return NULL;
	__drm_atomic_helper_plane_duplicate_state(plane, &state->base);
	/* A duplicate owns no pin; check and prepare establish its own plan. */
	return &state->base;
}

static int graphic_plane_check_bounds(struct drm_plane_state *state)
{
	struct graphic_plane_state *gstate = graphic_plane_state(state);
	struct drm_gem_object *obj = drm_gem_fb_get_obj(state->fb, 0);
	u64 x = state->src_x >> 16, y = state->src_y >> 16;
	u64 width = state->src_w >> 16, height = state->src_h >> 16;
	u64 offset, last_row, bytes;
	u32 pitch = state->fb->pitches[0];

	if (!obj || !width || !height)
		return -EINVAL;
	if (check_mul_overflow(y, (u64)pitch, &offset) ||
	    check_mul_overflow(x, 4ULL, &bytes) ||
	    check_add_overflow(offset, bytes, &offset) ||
	    check_add_overflow(offset, (u64)state->fb->offsets[0], &offset) ||
	    check_mul_overflow(height - 1, (u64)pitch, &last_row) ||
	    check_add_overflow(offset, last_row, &last_row) ||
	    check_mul_overflow(width, 4ULL, &bytes) ||
	    check_add_overflow(last_row, bytes, &gstate->end))
		return -EOVERFLOW;
	if (gstate->end > obj->size)
		return -EINVAL;
	gstate->offset = offset;
	return 0;
}

static int graphic_plane_check(struct drm_plane *plane,
			       struct drm_atomic_commit *commit)
{
	struct drm_plane_state *state;
	struct drm_crtc_state *crtc_state;
	bool cursor = plane->type == DRM_PLANE_TYPE_CURSOR;
	int ret;

	state = drm_atomic_get_new_plane_state(commit, plane);
	if (!state->fb || !state->crtc) {
		state->visible = false;
		return 0;
	}
	crtc_state = drm_atomic_get_crtc_state(commit, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  cursor, true);
	if (ret)
		return ret;
	if (state->src_x & 0xffff || state->src_y & 0xffff ||
	    state->src_w & 0xffff || state->src_h & 0xffff ||
	    state->fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    state->fb->format->num_planes != 1 || state->fb->format->cpp[0] != 4 ||
	    state->fb->pitches[0] % LS7A2000_STRIDE_ALIGN)
		return -EINVAL;

	if (cursor) {
		/* The backend currently accepts one packed 64x64 cursor image. */
		if (state->fb->width != LS7A2000_CURSOR_SIZE ||
		    state->fb->height != LS7A2000_CURSOR_SIZE ||
		    state->fb->pitches[0] != LS7A2000_CURSOR_SIZE * 4 ||
		    state->src_x || state->src_y ||
		    state->src_w != LS7A2000_CURSOR_SIZE << 16 ||
		    state->src_h != LS7A2000_CURSOR_SIZE << 16)
			return -EINVAL;
		/* Entirely offscreen cursors are hidden, with no coordinate encoding. */
		if (state->visible &&
		    (state->crtc_x < -63 || state->crtc_y < -63 ||
		     state->crtc_x > 4095 || state->crtc_y > 4095))
			return -EINVAL;
	} else if (crtc_state->enable &&
		   (state->crtc_x || state->crtc_y ||
		    state->crtc_w != crtc_state->mode.hdisplay ||
		    state->crtc_h != crtc_state->mode.vdisplay)) {
		return -EINVAL;
	}
	return graphic_plane_check_bounds(state);
}

static int graphic_plane_prepare_fb(struct drm_plane *plane,
				    struct drm_plane_state *state)
{
	struct graphic_plane_state *gstate = graphic_plane_state(state);
	struct drm_gem_object *obj;
	u64 base, first, last;
	int ret;

	if (!state->fb)
		return 0;
	obj = drm_gem_fb_get_obj(state->fb, 0);
	ret = graphic_bo_pin(obj, &base);
	if (ret)
		return ret;
	gstate->owns_pin = true;
	if (!gstate->end || gstate->offset >= gstate->end ||
	    check_add_overflow(base, gstate->offset, &first) ||
	    check_add_overflow(base, gstate->end - 1, &last) || last > U32_MAX) {
		ret = -ERANGE;
		goto unpin;
	}
	gstate->bus_address = first;
	ret = drm_gem_plane_helper_prepare_fb(plane, state);
	if (ret)
		goto unpin;
	return 0;

unpin:
	graphic_plane_cleanup_fb(plane, state);
	return ret;
}

static bool graphic_primary_unchanged(struct drm_plane_state *old,
				      struct drm_plane_state *new)
{
	return old->fb == new->fb && old->crtc == new->crtc &&
	       old->visible == new->visible &&
	       old->crtc_x == new->crtc_x && old->crtc_y == new->crtc_y &&
	       old->crtc_w == new->crtc_w && old->crtc_h == new->crtc_h &&
	       old->src_x == new->src_x && old->src_y == new->src_y &&
	       old->src_w == new->src_w && old->src_h == new->src_h &&
	       old->rotation == new->rotation && old->alpha == new->alpha &&
	       old->pixel_blend_mode == new->pixel_blend_mode &&
	       old->color_encoding == new->color_encoding &&
	       old->color_range == new->color_range;
}

bool graphic_cursor_position_only(struct graphic_pipe *pipe,
				  struct drm_atomic_commit *commit)
{
	struct drm_crtc_state *crtc_state;
	struct drm_plane_state *old, *new;

	if (!commit->legacy_cursor_update || READ_ONCE(pipe->fault) ||
	    READ_ONCE(pipe->force_retirement))
		return false;
	crtc_state = drm_atomic_get_new_crtc_state(commit, &pipe->crtc);
	if (!crtc_state || !crtc_state->active || crtc_state->event ||
	    drm_atomic_crtc_needs_modeset(crtc_state) ||
	    !ls7a2000_scanout_enabled(&pipe->gdev->hw, pipe->index))
		return false;
	old = drm_atomic_get_old_plane_state(commit, &pipe->primary.base);
	new = drm_atomic_get_new_plane_state(commit, &pipe->primary.base);
	if (!old || !new || !graphic_primary_unchanged(old, new))
		return false;
	old = drm_atomic_get_old_plane_state(commit, &pipe->cursor.base);
	new = drm_atomic_get_new_plane_state(commit, &pipe->cursor.base);
	/* FB identity also fixes BO, offset, pitch, dimensions and format. */
	if (!old || !new || !old->fb || old->fb != new->fb ||
	    old->crtc != &pipe->crtc || new->crtc != &pipe->crtc ||
	    old->visible != new->visible ||
	    old->crtc_w != new->crtc_w || old->crtc_h != new->crtc_h ||
	    old->src_x != new->src_x || old->src_y != new->src_y ||
	    old->src_w != new->src_w || old->src_h != new->src_h ||
	    old->rotation != new->rotation || old->alpha != new->alpha ||
	    old->pixel_blend_mode != new->pixel_blend_mode ||
	    old->color_encoding != new->color_encoding ||
	    old->color_range != new->color_range)
		return false;
	/* The new owner retains the same pinned storage after old-state cleanup. */
	return graphic_plane_state(old)->owns_pin &&
	       graphic_plane_state(new)->owns_pin &&
	       graphic_plane_state(old)->bus_address ==
		graphic_plane_state(new)->bus_address;
}

static void graphic_primary_update(struct drm_plane *plane,
				   struct drm_atomic_commit *commit)
{
	struct graphic_pipe *pipe = graphic_plane_pipe(plane);
	struct ls7a2000_hw *hw = &pipe->gdev->hw;
	struct drm_plane_state *old, *state;
	struct drm_crtc_state *crtc_state;
	struct graphic_plane_state *gstate;
	u8 bank;

	state = drm_atomic_get_new_plane_state(commit, plane);
	if (!state->fb || !state->visible)
		return;
	crtc_state = drm_atomic_get_new_crtc_state(commit, state->crtc);
	if (!crtc_state->active)
		return;
	gstate = graphic_plane_state(state);
	if (drm_atomic_crtc_needs_modeset(crtc_state) ||
	    !ls7a2000_scanout_enabled(hw, pipe->index)) {
		ls7a2000_scanout_prepare(hw, pipe->index, gstate->bus_address,
					 state->fb->pitches[0]);
		return;
	}
	old = drm_atomic_get_old_plane_state(commit, plane);
	if (graphic_primary_unchanged(old, state))
		return;
	bank = ls7a2000_scanout_flip(hw, pipe->index, gstate->bus_address);
	graphic_flip_expect_bank(pipe, bank);
}

static void graphic_cursor_program(struct graphic_pipe *pipe,
				   struct drm_plane_state *state)
{
	if (!state->fb || !state->visible) {
		ls7a2000_cursor_hide(&pipe->gdev->hw, pipe->index);
		return;
	}
	/* Keep the image base: the backend encodes clipping with the hotspot. */
	ls7a2000_cursor_show(&pipe->gdev->hw, pipe->index,
			     graphic_plane_state(state)->bus_address,
			     state->crtc_x, state->crtc_y);
}

static void graphic_cursor_update(struct drm_plane *plane,
				  struct drm_atomic_commit *commit)
{
	struct graphic_pipe *pipe = graphic_plane_pipe(plane);
	struct drm_plane_state *state;
	struct drm_crtc_state *crtc_state;

	state = drm_atomic_get_new_plane_state(commit, plane);
	crtc_state = drm_atomic_get_new_crtc_state(commit, &pipe->crtc);
	if (graphic_cursor_position_only(pipe, commit)) {
		if (state->visible)
			ls7a2000_cursor_move(&pipe->gdev->hw, pipe->index,
					     state->crtc_x, state->crtc_y);
		return;
	}
	if (!crtc_state->active)
		ls7a2000_cursor_hide(&pipe->gdev->hw, pipe->index);
	else
		graphic_cursor_program(pipe, state);
}

static void graphic_cursor_disable(struct drm_plane *plane,
				   struct drm_atomic_commit *commit)
{
	struct graphic_pipe *pipe = graphic_plane_pipe(plane);

	ls7a2000_cursor_hide(&pipe->gdev->hw, pipe->index);
}

void graphic_plane_restore(struct graphic_pipe *pipe,
			   struct drm_atomic_commit *commit)
{
	struct drm_plane_state *primary, *cursor;

	primary = drm_atomic_get_new_plane_state(commit, &pipe->primary.base);
	cursor = drm_atomic_get_new_plane_state(commit, &pipe->cursor.base);
	/* Atomic check included every affected plane and prepare pinned them. */
	ls7a2000_scanout_prepare(&pipe->gdev->hw, pipe->index,
				 graphic_plane_state(primary)->bus_address,
				 primary->fb->pitches[0]);
	graphic_cursor_program(pipe, cursor);
}

static const struct drm_plane_funcs graphic_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = graphic_plane_reset,
	.atomic_duplicate_state = graphic_plane_duplicate_state,
	.atomic_destroy_state = graphic_plane_destroy_state,
};

static const struct drm_plane_helper_funcs graphic_primary_helpers = {
	.prepare_fb = graphic_plane_prepare_fb,
	.cleanup_fb = graphic_plane_cleanup_fb,
	.atomic_check = graphic_plane_check,
	.atomic_update = graphic_primary_update,
};

static const struct drm_plane_helper_funcs graphic_cursor_helpers = {
	.prepare_fb = graphic_plane_prepare_fb,
	.cleanup_fb = graphic_plane_cleanup_fb,
	.atomic_check = graphic_plane_check,
	.atomic_update = graphic_cursor_update,
	.atomic_disable = graphic_cursor_disable,
};

int graphic_plane_init(struct graphic_pipe *pipe)
{
	static const u32 primary_formats[] = { DRM_FORMAT_XRGB8888 };
	static const u32 cursor_formats[] = { DRM_FORMAT_ARGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR,
					DRM_FORMAT_MOD_INVALID };
	struct drm_device *drm = &pipe->gdev->drm;
	int ret;

	pipe->primary.pipe = pipe;
	pipe->cursor.pipe = pipe;
	ret = drm_universal_plane_init(drm, &pipe->primary.base, BIT(pipe->index),
				       &graphic_plane_funcs, primary_formats,
				       ARRAY_SIZE(primary_formats), modifiers,
				       DRM_PLANE_TYPE_PRIMARY, "graphic-primary-%u",
				       pipe->index);
	if (ret)
		return ret;
	drm_plane_helper_add(&pipe->primary.base, &graphic_primary_helpers);
	ret = drm_universal_plane_init(drm, &pipe->cursor.base, BIT(pipe->index),
				       &graphic_plane_funcs, cursor_formats,
				       ARRAY_SIZE(cursor_formats), modifiers,
				       DRM_PLANE_TYPE_CURSOR, "graphic-cursor-%u",
				       pipe->index);
	if (ret)
		return ret;
	drm_plane_helper_add(&pipe->cursor.base, &graphic_cursor_helpers);
	return drm_plane_create_blend_mode_property(&pipe->cursor.base,
						  BIT(DRM_MODE_BLEND_COVERAGE));
}
