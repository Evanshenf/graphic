/* SPDX-License-Identifier: GPL-2.0 */
/* Private to the display layer. No PCI discovery, TTM internals or MMIO here. */
#ifndef GRAPHIC_DISPLAY_H
#define GRAPHIC_DISPLAY_H

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

#include "hardware/ls7a2000.h"

struct graphic_device;
struct graphic_pipe;
struct drm_atomic_commit;

struct graphic_plane {
	struct drm_plane base;
	struct graphic_pipe *pipe;
};

/* Every hardware update has a completion, including eventless cursor updates. */
struct graphic_flip {
	/* Protects transaction publication and exclusive IRQ/cancel ownership. */
	spinlock_t lock;
	struct completion done;
	struct drm_pending_vblank_event *event;
	u32 start_vblank;
	u8 target_bank;
	bool pending;
	bool require_bank;
	bool holds_vblank_ref;
};

struct graphic_pipe {
	struct graphic_device *gdev;
	unsigned int index;
	struct drm_crtc crtc;
	struct graphic_plane primary;
	struct graphic_plane cursor;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct ls7a2000_link firmware;
	struct drm_display_mode mode;
	bool mode_valid;
	bool fault;
	/* A newly started/recovered pipe must retire its first hardware plan. */
	bool force_retirement;
	/* Serializes complete hardware plans and their retirement, never IRQs. */
	struct mutex commit_lock;
	struct graphic_flip flip;
};

struct graphic_kms {
	struct graphic_device *gdev;
	struct graphic_pipe pipes[LS7A2000_PIPE_COUNT];
	bool irq_requested;
	bool poll_started;
};

static inline struct graphic_pipe *graphic_crtc_pipe(struct drm_crtc *crtc)
{
	return container_of(crtc, struct graphic_pipe, crtc);
}

static inline struct graphic_pipe *graphic_plane_pipe(struct drm_plane *plane)
{
	return container_of(plane, struct graphic_plane, base)->pipe;
}

static inline struct graphic_pipe *
graphic_connector_pipe(struct drm_connector *connector)
{
	return container_of(connector, struct graphic_pipe, connector);
}

int graphic_plane_init(struct graphic_pipe *pipe);
bool graphic_cursor_position_only(struct graphic_pipe *pipe,
				  struct drm_atomic_commit *commit);
/* Replay a fully checked/prepared plane plan while the pipe is disabled. */
void graphic_plane_restore(struct graphic_pipe *pipe,
			   struct drm_atomic_commit *commit);
int graphic_crtc_init(struct graphic_pipe *pipe);
void graphic_crtc_stop(struct graphic_pipe *pipe);
/* Runtime recovery uses the full plane plan, not a stale check-time fault bit. */
void graphic_crtc_recover(struct graphic_pipe *pipe,
			  struct drm_atomic_commit *commit);
int graphic_output_init(struct graphic_pipe *pipe);
bool graphic_output_mode_matches(struct graphic_pipe *pipe,
				 const struct drm_display_mode *mode);

void graphic_flip_init(struct graphic_pipe *pipe);
void graphic_flip_begin(struct graphic_pipe *pipe);
void graphic_flip_expect_bank(struct graphic_pipe *pipe, u8 bank);
void graphic_flip_arm(struct graphic_pipe *pipe, struct drm_crtc_state *state);
void graphic_flip_complete_immediate(struct graphic_pipe *pipe,
				     struct drm_crtc_state *state);
void graphic_flip_irq(struct graphic_pipe *pipe);
void graphic_flip_wait(struct graphic_pipe *pipe);
/* Process context: stop and drain before releasing completion or references. */
void graphic_flip_cancel(struct graphic_pipe *pipe);

#endif
