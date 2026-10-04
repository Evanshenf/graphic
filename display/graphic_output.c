// SPDX-License-Identifier: GPL-2.0
/* Connector policy: advertise only the firmware mode captured by the backend. */
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "graphic_device.h"
#include "graphic_display.h"

bool graphic_output_mode_matches(struct graphic_pipe *pipe,
				 const struct drm_display_mode *mode)
{
	/* drm_mode_equal() alone rounds the clock period to picoseconds. */
	return pipe->mode_valid && mode->clock == pipe->mode.clock &&
	       drm_mode_equal(mode, &pipe->mode);
}

static enum drm_connector_status
graphic_output_detect(struct drm_connector *connector, bool force)
{
	struct graphic_pipe *pipe = graphic_connector_pipe(connector);

	return ls7a2000_connected(&pipe->gdev->hw, pipe->index) ?
		connector_status_connected : connector_status_disconnected;
}

static int graphic_output_get_modes(struct drm_connector *connector)
{
	struct graphic_pipe *pipe = graphic_connector_pipe(connector);
	struct drm_display_mode *mode;

	if (!pipe->mode_valid)
		return 0;
	mode = drm_mode_duplicate(connector->dev, &pipe->mode);
	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static enum drm_mode_status
graphic_output_mode_valid(struct drm_connector *connector,
			  const struct drm_display_mode *mode)
{
	struct graphic_pipe *pipe = graphic_connector_pipe(connector);

	if (!pipe->mode_valid)
		return MODE_NOMODE;
	if (mode->clock != pipe->mode.clock)
		return MODE_CLOCK_RANGE;
	return graphic_output_mode_matches(pipe, mode) ? MODE_OK : MODE_BAD;
}

static void graphic_output_capture(struct graphic_pipe *pipe)
{
	struct ls7a2000_timing *timing = &pipe->firmware.timing;
	struct drm_display_mode *mode = &pipe->mode;
	struct drm_device *drm = &pipe->gdev->drm;
	int ret;

	ret = ls7a2000_capture_link(&pipe->gdev->hw, pipe->index, &pipe->firmware);
	if (ret) {
		drm_info(drm, "HDMI%u: no supported inherited mode (%d)\n",
			 pipe->index, ret);
		return;
	}
	if (pipe->firmware.pll_out_of_spec)
		drm_warn(drm,
			 "HDMI%u: inherited PLL outside manual range (ref=%u, loop=%u, out=%u); no retuning\n",
			 pipe->index, pipe->firmware.pixel_ref,
			 pipe->firmware.pixel_loop, pipe->firmware.pixel_out);
	mode->clock = timing->clock_khz;
	mode->hdisplay = timing->hdisplay;
	mode->hsync_start = timing->hsync_start;
	mode->hsync_end = timing->hsync_end;
	mode->htotal = timing->htotal;
	mode->vdisplay = timing->vdisplay;
	mode->vsync_start = timing->vsync_start;
	mode->vsync_end = timing->vsync_end;
	mode->vtotal = timing->vtotal;
	/* Hardware preserves inherited sync inversion; no guessed polarity. */
	mode->flags = 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	pipe->mode_valid = true;
}

static const struct drm_connector_funcs graphic_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = graphic_output_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs graphic_connector_helpers = {
	.get_modes = graphic_output_get_modes,
	.mode_valid = graphic_output_mode_valid,
};

static const struct drm_encoder_funcs graphic_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int graphic_output_init(struct graphic_pipe *pipe)
{
	struct drm_device *drm = &pipe->gdev->drm;
	int ret;

	graphic_output_capture(pipe);
	ret = drm_encoder_init(drm, &pipe->encoder, &graphic_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, "graphic-hdmi-%u", pipe->index);
	if (ret)
		return ret;
	pipe->encoder.possible_crtcs = BIT(pipe->index);
	ret = drm_connector_init(drm, &pipe->connector, &graphic_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	pipe->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				 DRM_CONNECTOR_POLL_DISCONNECT;
	drm_connector_helper_add(&pipe->connector, &graphic_connector_helpers);
	return drm_connector_attach_encoder(&pipe->connector, &pipe->encoder);
}
