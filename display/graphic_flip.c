// SPDX-License-Identifier: GPL-2.0
/* Hardware retirement is independent of userspace event allocation. */
#include <linux/jiffies.h>

#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "graphic_device.h"
#include "graphic_display.h"

static void graphic_flip_finish(struct graphic_pipe *pipe,
				struct drm_pending_vblank_event *event,
				bool vblank_ref)
{
	struct drm_device *drm = &pipe->gdev->drm;
	unsigned long flags;

	/* Never nest the transaction lock with DRM's event/vblank locks. */
	if (event) {
		spin_lock_irqsave(&drm->event_lock, flags);
		drm_crtc_send_vblank_event(&pipe->crtc, event);
		spin_unlock_irqrestore(&drm->event_lock, flags);
	}
	if (vblank_ref)
		drm_crtc_vblank_put(&pipe->crtc);
	WRITE_ONCE(pipe->force_retirement, false);
	/* Cleanup may release the old BO as soon as this completion is visible. */
	complete_all(&pipe->flip.done);
}

void graphic_flip_init(struct graphic_pipe *pipe)
{
	spin_lock_init(&pipe->flip.lock);
	init_completion(&pipe->flip.done);
	complete_all(&pipe->flip.done);
}

void graphic_flip_cancel(struct graphic_pipe *pipe)
{
	struct graphic_flip *flip = &pipe->flip;
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	bool vblank_ref;

	spin_lock_irqsave(&flip->lock, flags);
	if (!flip->pending) {
		spin_unlock_irqrestore(&flip->lock, flags);
		return;
	}
	/* Claim before sleeping: an IRQ must not retire BOs during the drain. */
	flip->pending = false;
	event = flip->event;
	flip->event = NULL;
	vblank_ref = flip->holds_vblank_ref;
	flip->holds_vblank_ref = false;
	WRITE_ONCE(pipe->fault, true);
	spin_unlock_irqrestore(&flip->lock, flags);

	graphic_crtc_stop(pipe);
	graphic_flip_finish(pipe, event, vblank_ref);
}

void graphic_flip_begin(struct graphic_pipe *pipe)
{
	struct graphic_flip *flip = &pipe->flip;
	unsigned long flags;

	/* The caller's commit_lock excludes any later transaction on this pipe. */
	graphic_flip_cancel(pipe);
	wait_for_completion(&flip->done);
	spin_lock_irqsave(&flip->lock, flags);
	reinit_completion(&flip->done);
	flip->require_bank = false;
	flip->event = NULL;
	flip->holds_vblank_ref = false;
	spin_unlock_irqrestore(&flip->lock, flags);
}

void graphic_flip_expect_bank(struct graphic_pipe *pipe, u8 bank)
{
	unsigned long flags;

	spin_lock_irqsave(&pipe->flip.lock, flags);
	pipe->flip.target_bank = bank;
	pipe->flip.require_bank = true;
	spin_unlock_irqrestore(&pipe->flip.lock, flags);
}

void graphic_flip_arm(struct graphic_pipe *pipe, struct drm_crtc_state *state)
{
	struct graphic_flip *flip = &pipe->flip;
	struct drm_pending_vblank_event *event = state->event;
	unsigned long flags;
	int ret;

	state->event = NULL;
	if (!state->active) {
		graphic_flip_finish(pipe, event, false);
		return;
	}
	if (READ_ONCE(pipe->fault)) {
		graphic_crtc_stop(pipe);
		graphic_flip_finish(pipe, event, false);
		return;
	}
	ret = drm_crtc_vblank_get(&pipe->crtc);
	if (ret) {
		WRITE_ONCE(pipe->fault, true);
		graphic_crtc_stop(pipe);
		drm_err(&pipe->gdev->drm, "pipe %u vblank unavailable: %d\n",
			pipe->index, ret);
		graphic_flip_finish(pipe, event, false);
		return;
	}

	spin_lock_irqsave(&flip->lock, flags);
	flip->start_vblank = ls7a2000_vblank_counter(&pipe->gdev->hw, pipe->index);
	flip->event = event;
	flip->holds_vblank_ref = true;
	flip->pending = true;
	spin_unlock_irqrestore(&flip->lock, flags);
}

void graphic_flip_complete_immediate(struct graphic_pipe *pipe,
				     struct drm_crtc_state *state)
{
	/* Requested page-flip events retain their normal vblank semantics. */
	if (state->event) {
		graphic_flip_arm(pipe, state);
		return;
	}
	/* A same-image position update retires no hardware buffer ownership. */
	graphic_flip_finish(pipe, NULL, false);
}

void graphic_flip_irq(struct graphic_pipe *pipe)
{
	struct graphic_flip *flip = &pipe->flip;
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	bool vblank_ref;
	u32 count;

	spin_lock_irqsave(&flip->lock, flags);
	if (!flip->pending)
		goto unlock;
	count = ls7a2000_vblank_counter(&pipe->gdev->hw, pipe->index);
	/* Unsigned equality also handles the counter wrapping at the next frame. */
	if (count == flip->start_vblank ||
	    (flip->require_bank &&
	     ls7a2000_scanout_bank(&pipe->gdev->hw, pipe->index) != flip->target_bank))
		goto unlock;
	flip->pending = false;
	event = flip->event;
	flip->event = NULL;
	vblank_ref = flip->holds_vblank_ref;
	flip->holds_vblank_ref = false;
	spin_unlock_irqrestore(&flip->lock, flags);
	graphic_flip_finish(pipe, event, vblank_ref);
	return;

unlock:
	spin_unlock_irqrestore(&flip->lock, flags);
}

void graphic_flip_wait(struct graphic_pipe *pipe)
{
	if (wait_for_completion_timeout(&pipe->flip.done, 10 * HZ))
		return;
	drm_err(&pipe->gdev->drm, "pipe %u flip timed out; stopping scanout\n",
		pipe->index);
	graphic_flip_cancel(pipe);
	/* An IRQ may already have claimed it and still be delivering the event. */
	wait_for_completion(&pipe->flip.done);
}
