// SPDX-License-Identifier: GPL-2.0
/* LS7A2000 user manual V1.3, sections 2.2.2, 4.9-4.10 and 20.2. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/math64.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "lsdc_drv.h"
#include "lsdc_output.h"

/* These offsets and fields are taken from the manual, not a driver table. */
#define OUT_PIPE_STRIDE		0x10
#define OUT_FB_CONFIG		0x1240
#define OUT_PANEL_CONFIG		0x13c0
#define OUT_HDISPLAY		0x1400
#define OUT_HSYNC		0x1420
#define OUT_VDISPLAY		0x1480
#define OUT_VSYNC		0x14a0
#define OUT_HDMI_CONTROL		0x1720
#define OUT_PHY_CONTROL		0x1800
#define OUT_PHY_PLL		0x1820
#define OUT_PIXEL_PLL_LOW	0x1a20
#define OUT_PIXEL_PLL_HIGH	0x1a40
#define OUT_SYNC_DEVIATION	0x1b80
#define OUT_HOTPLUG_STATUS	0x1ba0

#define OUT_FB_CROSS_ROUTE	BIT(9)
#define OUT_PANEL_ENABLED	(BIT(8) | BIT(0))
#define OUT_SYNC_ENABLED		BIT(30)
#define OUT_SYNC_DEVIATION_EN	BIT(31)
#define OUT_HORIZONTAL_LOW	GENMASK(12, 0)
#define OUT_HORIZONTAL_HIGH	GENMASK(28, 16)
#define OUT_VERTICAL_LOW		GENMASK(11, 0)
#define OUT_VERTICAL_HIGH	GENMASK(27, 16)
#define OUT_HDMI_ENABLED		BIT(0)

#define OUT_PHY_ENABLED		BIT(0)
#define OUT_PHY_RESET_N		BIT(1)
#define OUT_PHY_CONTROL_MASK	(BIT(0) | BIT(1) | BIT(2) | BIT(3) | \
				 GENMASK(9, 8) | BIT(10) | GENMASK(13, 12))
#define OUT_PHY_PLL_ENABLED	BIT(0)
#define OUT_PHY_PLL_LOCKED	BIT(16)
#define OUT_PHY_PLL_BYPASS	BIT(17)
#define OUT_PHY_PLL_CONTROL_MASK	(GENMASK(15, 0) | BIT(17))

#define OUT_PIXEL_DIVOUT		GENMASK(6, 0)
#define OUT_PIXEL_LOOPC		GENMASK(29, 21)
#define OUT_PIXEL_DIVREF		GENMASK(6, 0)
#define OUT_PIXEL_LOCKED		BIT(7)
#define OUT_PIXEL_SELECTED	BIT(8)
#define OUT_PIXEL_BYPASS		BIT(12)
#define OUT_PIXEL_POWERDOWN	BIT(13)
#define OUT_PIXEL_LOCAL_CONFIG	BIT(14)
#define OUT_PIXEL_REF_KHZ		100000

static u32 lsdc_output_read(struct lsdc_display_pipe *pipe, u32 reg)
{
	return lsdc_read(pipe->dev_priv, reg + pipe->index * OUT_PIPE_STRIDE);
}

static void lsdc_output_update(struct lsdc_display_pipe *pipe, u32 reg,
			       u32 mask, u32 value)
{
	u32 offset = reg + pipe->index * OUT_PIPE_STRIDE;
	u32 value_old = lsdc_read(pipe->dev_priv, offset);

	lsdc_write(pipe->dev_priv, offset, (value_old & ~mask) | (value & mask));
}

static int lsdc_output_pixel_clock(struct lsdc_display_pipe *pipe, bool warn)
{
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	u32 low, high, div_ref, div_out, loopc, divisor;
	u64 vco_numerator;

	high = lsdc_output_read(pipe, OUT_PIXEL_PLL_HIGH);
	if (high & OUT_PIXEL_LOCAL_CONFIG) {
		low = lsdc_output_read(pipe, OUT_PIXEL_PLL_LOW);
	} else {
		if (!dev_priv->pll_regs)
			return -ENODEV;
		low = readl(dev_priv->pll_regs + pipe->index * OUT_PIPE_STRIDE);
		high = readl(dev_priv->pll_regs +
			     pipe->index * OUT_PIPE_STRIDE + sizeof(u32));
	}

	if (!(high & OUT_PIXEL_LOCKED) || !(high & OUT_PIXEL_SELECTED) ||
	    high & (OUT_PIXEL_BYPASS | OUT_PIXEL_POWERDOWN))
		return -EINVAL;

	div_ref = FIELD_GET(OUT_PIXEL_DIVREF, high);
	div_out = FIELD_GET(OUT_PIXEL_DIVOUT, low);
	loopc = FIELD_GET(OUT_PIXEL_LOOPC, low);
	if (!div_ref || !div_out || !loopc)
		return -EINVAL;

	/*
	 * Manual section 2.2.2: input 20-40 MHz, VCO 1.8-4.8 GHz.
	 * A locked inherited configuration can fall outside these ranges.
	 * Report that limitation; this driver never programs the pixel PLL.
	 */
	vco_numerator = (u64)OUT_PIXEL_REF_KHZ * loopc;
	if (warn && (OUT_PIXEL_REF_KHZ < 20000 * div_ref ||
		     OUT_PIXEL_REF_KHZ > 40000 * div_ref ||
		     vco_numerator < (u64)1800000 * div_ref ||
		     vco_numerator > (u64)4800000 * div_ref))
		drm_warn(&dev_priv->drm_dev,
			 "HDMI%u: inherited locked pixel PLL outside manual range (ref=%u, loop=%u, out=%u); no retuning\n",
			 pipe->index, div_ref, loopc, div_out);

	divisor = div_ref * div_out;
	return div_u64(vco_numerator + divisor / 2, divisor);
}

static bool lsdc_output_capture(struct lsdc_display_pipe *pipe)
{
	struct lsdc_output *output = &pipe->output;
	struct drm_display_mode *mode = &output->boot_mode;
	u32 hdisplay, hsync, vdisplay, vsync, config;
	int clock;

	output->panel_config = lsdc_output_read(pipe, OUT_PANEL_CONFIG);
	output->hdmi_control = lsdc_output_read(pipe, OUT_HDMI_CONTROL);
	output->phy_control = lsdc_output_read(pipe, OUT_PHY_CONTROL);
	output->phy_pll = lsdc_output_read(pipe, OUT_PHY_PLL);
	config = lsdc_output_read(pipe, OUT_FB_CONFIG);
	/* DPMS may disable the output while retaining its timing and PLLs. */
	if (config & OUT_FB_CROSS_ROUTE ||
	    !(output->phy_pll & OUT_PHY_PLL_ENABLED) ||
	    !(output->phy_pll & OUT_PHY_PLL_LOCKED) ||
	    output->phy_pll & OUT_PHY_PLL_BYPASS ||
	    lsdc_output_read(pipe, OUT_SYNC_DEVIATION) & OUT_SYNC_DEVIATION_EN)
		return false;

	hdisplay = lsdc_output_read(pipe, OUT_HDISPLAY);
	hsync = lsdc_output_read(pipe, OUT_HSYNC);
	vdisplay = lsdc_output_read(pipe, OUT_VDISPLAY);
	vsync = lsdc_output_read(pipe, OUT_VSYNC);
	if (!(hsync & OUT_SYNC_ENABLED) || !(vsync & OUT_SYNC_ENABLED))
		return false;

	clock = lsdc_output_pixel_clock(pipe, true);
	if (clock <= 0)
		return false;

	mode->clock = clock;
	mode->hdisplay = FIELD_GET(OUT_HORIZONTAL_LOW, hdisplay);
	mode->htotal = FIELD_GET(OUT_HORIZONTAL_HIGH, hdisplay);
	mode->hsync_start = FIELD_GET(OUT_HORIZONTAL_LOW, hsync);
	mode->hsync_end = FIELD_GET(OUT_HORIZONTAL_HIGH, hsync);
	mode->vdisplay = FIELD_GET(OUT_VERTICAL_LOW, vdisplay);
	mode->vtotal = FIELD_GET(OUT_VERTICAL_HIGH, vdisplay);
	mode->vsync_start = FIELD_GET(OUT_VERTICAL_LOW, vsync);
	mode->vsync_end = FIELD_GET(OUT_VERTICAL_HIGH, vsync);

	if (!mode->hdisplay || mode->hdisplay > LSDC_MAX_WIDTH ||
	    mode->hdisplay > mode->hsync_start ||
	    mode->hsync_start >= mode->hsync_end ||
	    mode->hsync_end > mode->htotal ||
	    !mode->vdisplay || mode->vdisplay > LSDC_MAX_HEIGHT ||
	    mode->vdisplay > mode->vsync_start ||
	    mode->vsync_start >= mode->vsync_end ||
	    mode->vsync_end > mode->vtotal)
		return false;

	/* Keep sync inversion in hardware; the manual gives no polarity base. */
	mode->flags = 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	return true;
}

static struct lsdc_output *connector_to_output(struct drm_connector *connector)
{
	return container_of(connector, struct lsdc_output, connector);
}

static enum drm_connector_status
lsdc_output_detect(struct drm_connector *connector, bool force)
{
	struct lsdc_display_pipe *pipe;
	u32 status;

	pipe = output_to_pipe(connector_to_output(connector));
	status = lsdc_read(pipe->dev_priv, OUT_HOTPLUG_STATUS);
	return status & BIT(pipe->index) ? connector_status_connected :
					 connector_status_disconnected;
}

static int lsdc_output_get_modes(struct drm_connector *connector)
{
	struct lsdc_output *output = connector_to_output(connector);
	struct drm_display_mode *mode;

	if (!output->boot_mode_valid)
		return 0;

	mode = drm_mode_duplicate(connector->dev, &output->boot_mode);
	if (!mode)
		return 0;

	drm_mode_probed_add(connector, mode);
	return 1;
}

static enum drm_mode_status
lsdc_output_mode_valid(struct drm_connector *connector,
		       const struct drm_display_mode *mode)
{
	struct lsdc_output *output = connector_to_output(connector);

	if (!output->boot_mode_valid)
		return MODE_NOMODE;
	/* drm_mode_equal() rounds clock periods to whole picoseconds. */
	if (mode->clock != output->boot_mode.clock)
		return MODE_CLOCK_RANGE;

	return drm_mode_equal(mode, &output->boot_mode) ? MODE_OK : MODE_BAD;
}

static const struct drm_connector_funcs lsdc_output_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = lsdc_output_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs lsdc_output_connector_helpers = {
	.get_modes = lsdc_output_get_modes,
	.mode_valid = lsdc_output_mode_valid,
};

static const struct drm_encoder_funcs lsdc_output_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int lsdc_output_init(struct lsdc_display_pipe *pipe)
{
	struct drm_device *drm = &pipe->dev_priv->drm_dev;
	struct lsdc_output *output = &pipe->output;
	int ret;

	if (pipe->index >= LSDC_NUM_PIPES)
		return -EINVAL;

	output->boot_mode_valid = lsdc_output_capture(pipe);
	if (!output->boot_mode_valid)
		drm_info(drm, "HDMI%u: no supported inherited mode\n",
			 pipe->index);

	ret = drm_encoder_init(drm, &output->encoder, &lsdc_output_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, "lsdc-hdmi-%u", pipe->index);
	if (ret)
		return ret;
	output->encoder.possible_crtcs = BIT(pipe->index);

	ret = drm_connector_init(drm, &output->connector,
				 &lsdc_output_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		goto cleanup_encoder;

	output->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				   DRM_CONNECTOR_POLL_DISCONNECT;
	drm_connector_helper_add(&output->connector,
				 &lsdc_output_connector_helpers);
	ret = drm_connector_attach_encoder(&output->connector, &output->encoder);
	if (ret)
		goto cleanup_connector;

	return 0;

cleanup_connector:
	drm_connector_cleanup(&output->connector);
cleanup_encoder:
	drm_encoder_cleanup(&output->encoder);
	return ret;
}

void lsdc_output_disable(struct lsdc_display_pipe *pipe)
{
	lsdc_output_update(pipe, OUT_HDMI_CONTROL, OUT_HDMI_ENABLED, 0);
	lsdc_output_update(pipe, OUT_PANEL_CONFIG, OUT_PANEL_ENABLED, 0);
	/* Keep the working PHY and pixel PLL running: no frequency programming. */
}

int lsdc_output_enable(struct lsdc_display_pipe *pipe,
		       const struct drm_display_mode *mode)
{
	struct lsdc_output *output = &pipe->output;
	struct drm_lsdc_private *dev_priv = pipe->dev_priv;
	u32 phy, pll, phy_control, status;
	int ret;

	if (!output->boot_mode_valid || mode->clock != output->boot_mode.clock ||
	    !drm_mode_equal(mode, &output->boot_mode))
		return -EINVAL;
	if (lsdc_output_pixel_clock(pipe, false) != output->boot_mode.clock)
		return -EIO;

	phy = lsdc_output_read(pipe, OUT_PHY_CONTROL);
	pll = lsdc_output_read(pipe, OUT_PHY_PLL);
	/* Keep the inherited PHY PLL as well; no divider or enable writes. */
	if ((pll & OUT_PHY_PLL_CONTROL_MASK) !=
	    (output->phy_pll & OUT_PHY_PLL_CONTROL_MASK))
		return -EIO;

	phy_control = output->phy_control | OUT_PHY_ENABLED | OUT_PHY_RESET_N;
	if ((phy & OUT_PHY_CONTROL_MASK) !=
	    (phy_control & OUT_PHY_CONTROL_MASK)) {
		lsdc_output_disable(pipe);
		lsdc_output_update(pipe, OUT_PHY_CONTROL,
				   OUT_PHY_ENABLED | OUT_PHY_RESET_N, 0);
		/* Manual section 20.2.40 requires a low pulse >= 1 us. */
		udelay(2);
		lsdc_output_update(pipe, OUT_PHY_CONTROL, OUT_PHY_CONTROL_MASK,
				   phy_control);
	}

	/* A bounded wait is driver policy; the manual supplies no lock timeout. */
	ret = readl_poll_timeout(dev_priv->ioregs + OUT_PHY_PLL +
				 pipe->index * OUT_PIPE_STRIDE, status,
				 status & OUT_PHY_PLL_LOCKED, 10, 100000);
	if (ret) {
		lsdc_output_disable(pipe);
		return ret;
	}

	lsdc_output_update(pipe, OUT_PANEL_CONFIG, GENMASK(9, 8) | GENMASK(1, 0),
			   output->panel_config | OUT_PANEL_ENABLED);
	lsdc_output_update(pipe, OUT_HDMI_CONTROL, GENMASK(9, 0),
			   output->hdmi_control | OUT_HDMI_ENABLED);
	return 0;
}
