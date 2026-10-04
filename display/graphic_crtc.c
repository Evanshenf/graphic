// SPDX-License-Identifier: GPL-2.0
/* CRTC lifecycle translates validated DRM state into backend operations. */
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "graphic_device.h"
#include "graphic_display.h"

static u32 graphic_crtc_vblank_counter(struct drm_crtc *crtc)
{
	struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);

	return ls7a2000_vblank_counter(&pipe->gdev->hw, pipe->index);
}

static int graphic_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);

	ls7a2000_vblank_enable(&pipe->gdev->hw, pipe->index);
	return 0;
}

static void graphic_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);

	ls7a2000_vblank_disable(&pipe->gdev->hw, pipe->index);
}

static int graphic_crtc_check(struct drm_crtc *crtc,
			      struct drm_atomic_commit *commit)
{
	struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);
	struct drm_crtc_state *state;
	struct drm_plane_state *primary;

	state = drm_atomic_get_new_crtc_state(commit, crtc);
	if (!state->enable)
		return 0;
	primary = drm_atomic_get_new_plane_state(commit, crtc->primary);
	if (!primary || primary->crtc != crtc || !primary->fb || !primary->visible)
		return -EINVAL;
	if (!graphic_output_mode_matches(pipe, &state->adjusted_mode))
		return -EINVAL;
	return drm_atomic_helper_check_crtc_primary_plane(state);
}

void graphic_crtc_stop(struct graphic_pipe *pipe)
{
	ls7a2000_pipe_disable(&pipe->gdev->hw, pipe->index, &pipe->firmware.timing);
	drm_crtc_vblank_off(&pipe->crtc);
}

static int graphic_crtc_start(struct graphic_pipe *pipe)
{
	int ret;

	/* Recovery may run before atomic_begin, so begin must preserve this. */
	WRITE_ONCE(pipe->force_retirement, true);
	ret = ls7a2000_pipe_enable(&pipe->gdev->hw, pipe->index, &pipe->firmware);
	if (ret) {
		WRITE_ONCE(pipe->fault, true);
		graphic_crtc_stop(pipe);
		drm_err(&pipe->gdev->drm, "pipe %u enable failed: %d\n",
			pipe->index, ret);
		return ret;
	}
	drm_crtc_vblank_on(&pipe->crtc);
	WRITE_ONCE(pipe->fault, false);
	return 0;
}

void graphic_crtc_recover(struct graphic_pipe *pipe,
			  struct drm_atomic_commit *commit)
{
	/* Caller holds commit_lock and supplied a complete prepared plane plan. */
	graphic_crtc_stop(pipe);
	graphic_plane_restore(pipe, commit);
	graphic_crtc_start(pipe);
}

static void graphic_crtc_begin(struct drm_crtc *crtc,
			       struct drm_atomic_commit *commit)
{
	graphic_flip_begin(graphic_crtc_pipe(crtc));
}

static void graphic_crtc_flush(struct drm_crtc *crtc,
			       struct drm_atomic_commit *commit)
{
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(commit, crtc);

	/* Modeset events and retirement start only after scanout/vblank are on. */
	if (state->active && drm_atomic_crtc_needs_modeset(state))
		return;
	if (graphic_cursor_position_only(graphic_crtc_pipe(crtc), commit)) {
		graphic_flip_complete_immediate(graphic_crtc_pipe(crtc), state);
		return;
	}
	graphic_flip_arm(graphic_crtc_pipe(crtc), state);
}

static void graphic_crtc_enable(struct drm_crtc *crtc,
				struct drm_atomic_commit *commit)
{
	struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(commit, crtc);

	graphic_crtc_start(pipe);
	graphic_flip_arm(pipe, state);
}

static void graphic_crtc_disable(struct drm_crtc *crtc,
				 struct drm_atomic_commit *commit)
{
	struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);

	graphic_flip_cancel(pipe);
	graphic_crtc_stop(pipe);
}

static const struct drm_crtc_funcs graphic_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.get_vblank_counter = graphic_crtc_vblank_counter,
	.enable_vblank = graphic_crtc_enable_vblank,
	.disable_vblank = graphic_crtc_disable_vblank,
};

static const struct drm_crtc_helper_funcs graphic_crtc_helpers = {
	.atomic_check = graphic_crtc_check,
	.atomic_begin = graphic_crtc_begin,
	.atomic_flush = graphic_crtc_flush,
	.atomic_enable = graphic_crtc_enable,
	.atomic_disable = graphic_crtc_disable,
};

int graphic_crtc_init(struct graphic_pipe *pipe)
{
	struct drm_device *drm = &pipe->gdev->drm;
	int ret;

	ret = drm_crtc_init_with_planes(drm, &pipe->crtc, &pipe->primary.base,
					&pipe->cursor.base, &graphic_crtc_funcs,
					"graphic-crtc-%u", pipe->index);
	if (ret)
		return ret;
	drm_crtc_helper_add(&pipe->crtc, &graphic_crtc_helpers);
	drm->max_vblank_count = U32_MAX;
	return 0;
}
