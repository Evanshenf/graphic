// SPDX-License-Identifier: GPL-2.0
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "lsdc_drv.h"
#include "lsdc_display_driver.h"
#include "lsdc_output.h"

static int lsdc_atomic_check(struct drm_device *drm,
			     struct drm_atomic_commit *commit)
{
	struct drm_plane *plane;
	struct drm_plane_state *old, *new;
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;

	/* Stride has no documented frame latch: change it with scanout stopped. */
	for_each_oldnew_plane_in_state(commit, plane, old, new, i) {
		if (plane->type != DRM_PLANE_TYPE_PRIMARY || !old->fb ||
		    !new->fb || !new->crtc ||
		    old->fb->pitches[0] == new->fb->pitches[0])
			continue;
		crtc_state = drm_atomic_get_crtc_state(commit, new->crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);
		crtc_state->mode_changed = true;
	}
	for_each_new_crtc_in_state(commit, crtc, crtc_state, i) {
		if (READ_ONCE(crtc_to_pipe(crtc)->scanout_fault))
			crtc_state->mode_changed = true;
	}
	return drm_atomic_helper_check(drm, commit);
}

static void lsdc_commit_tail(struct drm_atomic_commit *commit)
{
	struct drm_device *drm = commit->dev;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	unsigned int i;

	drm_atomic_helper_commit_modeset_disables(drm, commit);
	drm_atomic_helper_commit_planes(drm, commit, 0);
	drm_atomic_helper_commit_modeset_enables(drm, commit);
	drm_atomic_helper_fake_vblank(commit);
	/* The IRQ completes flips only after observing the requested FB bank. */
	drm_atomic_helper_wait_for_flip_done(drm, commit);
	for_each_new_crtc_in_state(commit, crtc, crtc_state, i) {
		if (crtc_state->active)
			lsdc_crtc_abort_pending(crtc);
	}
	/* Keep later commits waiting until any fault has finished stopping DMA. */
	drm_atomic_helper_commit_hw_done(commit);
	drm_atomic_helper_cleanup_planes(drm, commit);
}

static const struct drm_mode_config_funcs lsdc_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = lsdc_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs lsdc_mode_config_helpers = {
	.atomic_commit_tail = lsdc_commit_tail,
};

static void lsdc_poll_cleanup(struct drm_device *drm, void *data)
{
	drm_kms_helper_poll_fini(drm);
}

int lsdc_display_driver_init(struct drm_lsdc_private *dev_priv)
{
	struct drm_device *drm = &dev_priv->drm_dev;
	unsigned int i;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.funcs = &lsdc_mode_config_funcs;
	drm->mode_config.helper_private = &lsdc_mode_config_helpers;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	/* A shared desktop framebuffer may span both 1080p outputs. */
	drm->mode_config.max_width = 2 * LSDC_MAX_WIDTH;
	drm->mode_config.max_height = 2 * LSDC_MAX_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.prefer_shadow = 1;
	drm->mode_config.cursor_width = 64;
	drm->mode_config.cursor_height = 64;

	for (i = 0; i < LSDC_NUM_PIPES; i++) {
		struct lsdc_display_pipe *pipe = &dev_priv->dispipe[i];

		pipe->dev_priv = dev_priv;
		pipe->index = i;
		/* Capture firmware configuration before any hardware writes. */
		ret = lsdc_output_init(pipe);
		if (ret)
			return ret;
		ret = lsdc_plane_init(pipe);
		if (ret)
			return ret;
		ret = lsdc_crtc_init(pipe);
		if (ret)
			return ret;
	}

	ret = drm_vblank_init(drm, LSDC_NUM_PIPES);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);
	return drmm_add_action_or_reset(drm, lsdc_poll_cleanup, NULL);
}
