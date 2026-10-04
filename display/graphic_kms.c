// SPDX-License-Identifier: GPL-2.0
/* DRM orchestration: complete plans, serialized retirement and managed objects. */
#include <linux/interrupt.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "graphic_device.h"
#include "graphic_display.h"
#include "graphic_kms.h"

static int graphic_atomic_check(struct drm_device *drm,
				struct drm_atomic_commit *commit)
{
	struct drm_plane_state *old, *new;
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	unsigned int i;
	int ret;

	/* Runtime recovery needs a complete plan, including a disabled cursor. */
	for_each_new_crtc_in_state(commit, crtc, crtc_state, i) {
		struct graphic_pipe *pipe = graphic_crtc_pipe(crtc);

		ret = drm_atomic_add_affected_planes(commit, crtc);
		if (ret)
			return ret;
		new = drm_atomic_get_plane_state(commit, &pipe->primary.base);
		if (IS_ERR(new))
			return PTR_ERR(new);
		new = drm_atomic_get_plane_state(commit, &pipe->cursor.base);
		if (IS_ERR(new))
			return PTR_ERR(new);
		if (READ_ONCE(pipe->fault))
			crtc_state->mode_changed = true;
	}
	/* Stride has no documented frame latch: change it with output stopped. */
	for_each_oldnew_plane_in_state(commit, plane, old, new, i) {
		if (plane->type != DRM_PLANE_TYPE_PRIMARY || !old->fb ||
		    !new->fb || !new->crtc ||
		    old->fb->pitches[0] == new->fb->pitches[0])
			continue;
		crtc_state = drm_atomic_get_new_crtc_state(commit, new->crtc);
		crtc_state->mode_changed = true;
	}
	/* Preset mode_changed makes this helper pull in affected connectors. */
	return drm_atomic_helper_check(drm, commit);
}

static void graphic_commit_tail(struct drm_atomic_commit *commit)
{
	struct drm_device *drm = commit->dev;
	struct graphic_kms *kms = to_graphic(drm)->kms;
	struct drm_crtc_state *state;
	unsigned int i, locked = 0;

	/* Generic dependency waits can time out; these locks still exclude HW. */
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		struct graphic_pipe *pipe = &kms->pipes[i];

		state = drm_atomic_get_new_crtc_state(commit, &pipe->crtc);
		if (!state)
			continue;
		mutex_lock(&pipe->commit_lock);
		locked |= BIT(i);
	}
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		struct graphic_pipe *pipe = &kms->pipes[i];

		if (!(locked & BIT(i)))
			continue;
		state = drm_atomic_get_new_crtc_state(commit, &pipe->crtc);
		/* A predecessor can fault after this commit has passed check. */
		if (state->active && READ_ONCE(pipe->fault) &&
		    !drm_atomic_crtc_needs_modeset(state))
			graphic_crtc_recover(pipe, commit);
	}

	drm_atomic_helper_commit_modeset_disables(drm, commit);
	drm_atomic_helper_commit_planes(drm, commit, 0);
	drm_atomic_helper_commit_modeset_enables(drm, commit);
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		if (locked & BIT(i))
			graphic_flip_wait(&kms->pipes[i]);
	}
	/* No hardware or permanent state changes are allowed after hw_done. */
	drm_atomic_helper_commit_hw_done(commit);
	drm_atomic_helper_cleanup_planes(drm, commit);
	for (i = LS7A2000_PIPE_COUNT; i-- > 0;) {
		if (locked & BIT(i))
			mutex_unlock(&kms->pipes[i].commit_lock);
	}
}

static const struct drm_mode_config_funcs graphic_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = graphic_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs graphic_mode_config_helpers = {
	.atomic_commit_tail = graphic_commit_tail,
};

static irqreturn_t graphic_irq_handler(int irq, void *data)
{
	struct graphic_kms *kms = data;
	u32 pending = ls7a2000_irq_read_and_ack(&kms->gdev->hw);
	unsigned int i;

	if (!pending)
		return IRQ_NONE;
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		if (!(pending & BIT(i)))
			continue;
		drm_crtc_handle_vblank(&kms->pipes[i].crtc);
		graphic_flip_irq(&kms->pipes[i]);
	}
	return IRQ_HANDLED;
}

static void graphic_poll_cleanup(struct drm_device *drm, void *data)
{
	struct graphic_kms *kms = data;

	if (!kms->poll_started)
		return;
	drm_kms_helper_poll_fini(drm);
	kms->poll_started = false;
}

int graphic_kms_init(struct graphic_device *gdev)
{
	struct drm_device *drm = &gdev->drm;
	struct graphic_kms *kms;
	unsigned int i;
	int ret;

	kms = drmm_kzalloc(drm, sizeof(*kms), GFP_KERNEL);
	if (!kms)
		return -ENOMEM;
	gdev->kms = kms;
	kms->gdev = gdev;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &graphic_mode_config_funcs;
	drm->mode_config.helper_private = &graphic_mode_config_helpers;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	/* A shared framebuffer can span both outputs. Each pipe remains 1080p. */
	drm->mode_config.max_width = LS7A2000_MAX_WIDTH * LS7A2000_PIPE_COUNT;
	drm->mode_config.max_height = LS7A2000_MAX_HEIGHT * LS7A2000_PIPE_COUNT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.prefer_shadow = 1;
	drm->mode_config.cursor_width = LS7A2000_CURSOR_SIZE;
	drm->mode_config.cursor_height = LS7A2000_CURSOR_SIZE;

	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		struct graphic_pipe *pipe = &kms->pipes[i];

		pipe->gdev = gdev;
		pipe->index = i;
		mutex_init(&pipe->commit_lock);
		graphic_flip_init(pipe);
		ret = graphic_output_init(pipe);
		if (ret)
			return ret;
		ret = graphic_plane_init(pipe);
		if (ret)
			return ret;
		ret = graphic_crtc_init(pipe);
		if (ret)
			return ret;
	}
	ret = drm_vblank_init(drm, LS7A2000_PIPE_COUNT);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		struct graphic_pipe *pipe = &kms->pipes[i];

		if (!pipe->crtc.state || !pipe->primary.base.state ||
		    !pipe->cursor.base.state || !pipe->connector.state)
			return -ENOMEM;
	}
	/* Register cleanup before starting anything that must be stopped. */
	ret = drmm_add_action_or_reset(drm, graphic_poll_cleanup, kms);
	if (ret)
		return ret;
	/* All firmware snapshots must precede the first hardware change. */
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		if (!kms->pipes[i].mode_valid &&
		    ls7a2000_scanout_enabled(&gdev->hw, i)) {
			drm_err(drm, "pipe %u: cannot drain an unknown firmware mode\n", i);
			return -ENODEV;
		}
	}
	ls7a2000_irq_disable_all(&gdev->hw);
	/* Software starts inactive; generic modeset disables cannot stop EFI. */
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++)
		graphic_crtc_stop(&kms->pipes[i]);
	ret = devm_request_irq(&gdev->pci->dev, gdev->pci->irq,
			       graphic_irq_handler, IRQF_SHARED, "graphic", kms);
	if (ret)
		return ret;
	kms->irq_requested = true;
	drm_kms_helper_poll_init(drm);
	kms->poll_started = true;
	return 0;
}

void graphic_kms_stop(struct graphic_device *gdev)
{
	struct graphic_kms *kms = gdev->kms;
	unsigned int i;

	graphic_poll_cleanup(&gdev->drm, kms);
	drm_atomic_helper_shutdown(&gdev->drm);
	/* Also quiesce hardware if shutdown could not allocate an atomic state. */
	for (i = 0; i < LS7A2000_PIPE_COUNT; i++) {
		struct graphic_pipe *pipe = &kms->pipes[i];

		mutex_lock(&pipe->commit_lock);
		graphic_flip_cancel(pipe);
		graphic_crtc_stop(pipe);
		mutex_unlock(&pipe->commit_lock);
	}
	ls7a2000_irq_disable_all(&gdev->hw);
	if (kms->irq_requested)
		synchronize_irq(gdev->pci->irq);
}
