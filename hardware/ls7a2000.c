// SPDX-License-Identifier: GPL-2.0
/* Hardware contract: LS7A2000 V1.3 sections 2.2.2, 4.9-4.10 and chapter 20. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/string.h>

#include "ls7a2000.h"
#include "ls7a2000_regs.h"

/* Manual 20.2.22-30: the two cursor blocks have irregular register spacing. */
struct cursor_registers {
	u16 config;
	u16 address;
	u16 address_high;
	u16 position;
};

static const struct cursor_registers cursor_regs[] = {
	{ 0x1520, 0x1530, 0x15e0, 0x1540 },
	{ 0x1670, 0x1680, 0x16e0, 0x1690 },
};

static u32 dc_read(struct ls7a2000_hw *hw, u32 reg)
{
	return readl(hw->dc + reg);
}

static void dc_write(struct ls7a2000_hw *hw, u32 reg, u32 value)
{
	writel(value, hw->dc + reg);
}

static u32 pipe_read(struct ls7a2000_hw *hw, unsigned int pipe, u32 reg)
{
	return dc_read(hw, DC_PIPE_REG(reg, pipe));
}

static void pipe_write(struct ls7a2000_hw *hw, unsigned int pipe, u32 reg,
		       u32 value)
{
	dc_write(hw, DC_PIPE_REG(reg, pipe), value);
}

static void pipe_update(struct ls7a2000_hw *hw, unsigned int pipe, u32 reg,
			u32 mask, u32 value)
{
	u32 old = pipe_read(hw, pipe, reg);

	pipe_write(hw, pipe, reg, (old & ~mask) | (value & mask));
}

void ls7a2000_hw_init(struct ls7a2000_hw *hw, void __iomem *dc,
		      void __iomem *pixel_pll)
{
	hw->dc = dc;
	hw->pixel_pll = pixel_pll;
	hw->irq_mask = 0;
	spin_lock_init(&hw->irq_lock);
}

static int pixel_clock(struct ls7a2000_hw *hw, unsigned int pipe,
		       struct ls7a2000_link *link)
{
	u32 low, high, ref, out, loop, divisor;
	u64 numerator;

	high = pipe_read(hw, pipe, DC_PIXEL_PLL_HIGH);
	if (high & PIXEL_LOCAL_CONFIG) {
		low = pipe_read(hw, pipe, DC_PIXEL_PLL_LOW);
	} else {
		if (!hw->pixel_pll)
			return -ENODEV;
		/* The bridge manual permits W accesses, not a single 64-bit access. */
		low = readl(hw->pixel_pll + pipe * 0x10);
		high = readl(hw->pixel_pll + pipe * 0x10 + sizeof(u32));
	}
	if (!(high & PIXEL_LOCKED) || !(high & PIXEL_SELECTED) ||
	    high & (PIXEL_BYPASS | PIXEL_POWERDOWN))
		return -EINVAL;

	ref = FIELD_GET(PIXEL_DIVREF, high);
	out = FIELD_GET(PIXEL_DIVOUT, low);
	loop = FIELD_GET(PIXEL_LOOP, low);
	if (!ref || !out || !loop)
		return -EINVAL;
	numerator = (u64)PIXEL_REFERENCE_KHZ * loop;
	if (link) {
		link->pixel_ref = ref;
		link->pixel_loop = loop;
		link->pixel_out = out;
		link->pll_out_of_spec = PIXEL_REFERENCE_KHZ < 20000 * ref ||
			PIXEL_REFERENCE_KHZ > 40000 * ref ||
			numerator < (u64)1800000 * ref ||
			numerator > (u64)4800000 * ref;
	}
	divisor = ref * out;
	return div_u64(numerator + divisor / 2, divisor);
}

static u64 frame_period_us(const struct ls7a2000_timing *timing)
{
	if (!timing || !timing->clock_khz)
		return 20000;
	return DIV_ROUND_UP_ULL((u64)timing->htotal * timing->vtotal * 1000,
			       timing->clock_khz);
}

/**
 * ls7a2000_capture_link - read the configured link without programming hardware
 * @hw: mapped register windows
 * @pipe: display unit, 0 or 1
 * @link: immutable snapshot returned on success
 *
 * Disabled outputs can retain useful timing. PLL range violations are returned
 * as metadata, not silently reprogrammed. A frame longer than one second is
 * rejected so subsequent conservative drain waits remain bounded.
 * Return: 0 on success, a negative error if the handoff is unsupported.
 */
int ls7a2000_capture_link(struct ls7a2000_hw *hw, unsigned int pipe,
			  struct ls7a2000_link *link)
{
	struct ls7a2000_timing *t = &link->timing;
	u32 hdisplay, hsync, vdisplay, vsync;
	int clock;

	if (pipe >= LS7A2000_PIPE_COUNT)
		return -EINVAL;
	memset(link, 0, sizeof(*link));
	link->panel = pipe_read(hw, pipe, DC_PANEL);
	link->hdmi = pipe_read(hw, pipe, DC_HDMI);
	link->phy = pipe_read(hw, pipe, DC_PHY);
	link->phy_pll = pipe_read(hw, pipe, DC_PHY_PLL);
	if (pipe_read(hw, pipe, DC_FB_CONFIG) & FB_CROSS_ROUTE ||
	    !(link->phy_pll & PHY_PLL_ENABLE) ||
	    !(link->phy_pll & PHY_PLL_LOCKED) ||
	    link->phy_pll & PHY_PLL_BYPASS ||
	    pipe_read(hw, pipe, DC_SYNC_DEVIATION) & SYNC_DEVIATION_ENABLE)
		return -EINVAL;

	hdisplay = pipe_read(hw, pipe, DC_HDISPLAY);
	hsync = pipe_read(hw, pipe, DC_HSYNC);
	vdisplay = pipe_read(hw, pipe, DC_VDISPLAY);
	vsync = pipe_read(hw, pipe, DC_VSYNC);
	if (!(hsync & SYNC_ENABLE) || !(vsync & SYNC_ENABLE))
		return -EINVAL;
	clock = pixel_clock(hw, pipe, link);
	if (clock <= 0)
		return -EINVAL;
	t->clock_khz = clock;
	t->hdisplay = FIELD_GET(H_COUNT_LOW, hdisplay);
	t->htotal = FIELD_GET(H_COUNT_HIGH, hdisplay);
	t->hsync_start = FIELD_GET(H_COUNT_LOW, hsync);
	t->hsync_end = FIELD_GET(H_COUNT_HIGH, hsync);
	t->vdisplay = FIELD_GET(V_COUNT_LOW, vdisplay);
	t->vtotal = FIELD_GET(V_COUNT_HIGH, vdisplay);
	t->vsync_start = FIELD_GET(V_COUNT_LOW, vsync);
	t->vsync_end = FIELD_GET(V_COUNT_HIGH, vsync);
	if (!t->hdisplay || t->hdisplay > LS7A2000_MAX_WIDTH ||
	    t->hdisplay > t->hsync_start || t->hsync_start >= t->hsync_end ||
	    t->hsync_end > t->htotal || !t->vdisplay ||
	    t->vdisplay > LS7A2000_MAX_HEIGHT || t->vdisplay > t->vsync_start ||
	    t->vsync_start >= t->vsync_end || t->vsync_end > t->vtotal)
		return -EINVAL;
	return frame_period_us(t) > 1000000 ? -ERANGE : 0;
}

bool ls7a2000_connected(struct ls7a2000_hw *hw, unsigned int pipe)
{
	return dc_read(hw, DC_HOTPLUG) & BIT(pipe);
}

static void link_disable(struct ls7a2000_hw *hw, unsigned int pipe)
{
	pipe_update(hw, pipe, DC_HDMI, HDMI_ENABLE, 0);
	pipe_update(hw, pipe, DC_PANEL, PANEL_ENABLE, 0);
}

static int link_enable(struct ls7a2000_hw *hw, unsigned int pipe,
		       const struct ls7a2000_link *link)
{
	u32 phy, desired, pll, status;
	int ret;

	if (pixel_clock(hw, pipe, NULL) != link->timing.clock_khz)
		return -EIO;
	phy = pipe_read(hw, pipe, DC_PHY);
	pll = pipe_read(hw, pipe, DC_PHY_PLL);
	if ((pll & PHY_PLL_CONTROLS) != (link->phy_pll & PHY_PLL_CONTROLS))
		return -EIO;
	desired = link->phy | PHY_ENABLE | PHY_RESET_N;
	if ((phy & PHY_CONTROLS) != (desired & PHY_CONTROLS)) {
		link_disable(hw, pipe);
		pipe_update(hw, pipe, DC_PHY, PHY_ENABLE | PHY_RESET_N, 0);
		/* Manual 20.2.40 requires a low pulse of at least 1 us. */
		udelay(2);
		pipe_update(hw, pipe, DC_PHY, PHY_CONTROLS, desired);
	}
	/* Engineering timeout; the manual does not specify maximum lock time. */
	ret = readl_poll_timeout(hw->dc + DC_PIPE_REG(DC_PHY_PLL, pipe), status,
				 status & PHY_PLL_LOCKED, 10, 100000);
	if (ret) {
		link_disable(hw, pipe);
		return ret;
	}
	pipe_update(hw, pipe, DC_PANEL, GENMASK(9, 8) | GENMASK(1, 0),
		    link->panel | PANEL_ENABLE);
	pipe_update(hw, pipe, DC_HDMI, GENMASK(9, 0), link->hdmi | HDMI_ENABLE);
	return 0;
}

/**
 * ls7a2000_pipe_enable - apply captured timings and enable prepared scanout
 * @hw: mapped windows
 * @pipe: display unit
 * @link: previously validated firmware snapshot
 *
 * Process context. Framebuffer addresses must already reference pinned BOs.
 * Neither pixel nor PHY PLL divisors are written. Sync inversion is preserved.
 * Return: 0 if the link is ready, a negative error if its clock has changed.
 */
int ls7a2000_pipe_enable(struct ls7a2000_hw *hw, unsigned int pipe,
			 const struct ls7a2000_link *link)
{
	const struct ls7a2000_timing *t = &link->timing;
	u32 sync, config;
	int ret;

	pipe_write(hw, pipe, DC_HDISPLAY, t->htotal << 16 | t->hdisplay);
	pipe_write(hw, pipe, DC_VDISPLAY, t->vtotal << 16 | t->vdisplay);
	sync = pipe_read(hw, pipe, DC_HSYNC) & SYNC_INVERT;
	pipe_write(hw, pipe, DC_HSYNC, sync | SYNC_ENABLE |
		   t->hsync_end << 16 | t->hsync_start);
	sync = pipe_read(hw, pipe, DC_VSYNC) & SYNC_INVERT;
	pipe_write(hw, pipe, DC_VSYNC, sync | SYNC_ENABLE |
		   t->vsync_end << 16 | t->vsync_start);
	pipe_write(hw, pipe, DC_DITHER, 0);
	config = pipe_read(hw, pipe, DC_FB_CONFIG);
	config &= ~(FB_FORMAT | FB_TILE | FB_GAMMA | FB_DMA | FB_CROSS_ROUTE |
		    FB_BANK | FB_SWITCH | FB_ENABLE);
	config |= FB_RESET | FB_ARGB8888;
	pipe_write(hw, pipe, DC_FB_CONFIG, config);
	ret = link_enable(hw, pipe, link);
	if (ret)
		return ret;
	/* Publish VRAM contents and timing registers before scanout starts. */
	wmb();
	pipe_write(hw, pipe, DC_FB_CONFIG, config | FB_ENABLE);
	return 0;
}

/**
 * ls7a2000_pipe_disable - stop scanout before memory ownership can change
 * @hw: mapped windows
 * @pipe: display unit
 * @timing: active timing, used to compute a full frame interval
 *
 * Process context; sleeps for a frame plus 1 ms after the disable readback.
 * This is a conservative drain policy, not a documented DMA-idle completion.
 */
void ls7a2000_pipe_disable(struct ls7a2000_hw *hw, unsigned int pipe,
			   const struct ls7a2000_timing *timing)
{
	u32 delay = frame_period_us(timing) + 1000;

	ls7a2000_cursor_hide(hw, pipe);
	pipe_update(hw, pipe, DC_FB_CONFIG, FB_ENABLE | FB_SWITCH, 0);
	pipe_read(hw, pipe, DC_FB_CONFIG);
	usleep_range(delay, delay + 1000);
	link_disable(hw, pipe);
}

void ls7a2000_scanout_prepare(struct ls7a2000_hw *hw, unsigned int pipe,
			      u32 bus_address, u32 pitch)
{
	pipe_write(hw, pipe, DC_FB_ADDRESS0, bus_address);
	pipe_write(hw, pipe, DC_FB_ADDRESS1, bus_address);
	pipe_write(hw, pipe, DC_FB_STRIDE, pitch);
	pipe_write(hw, pipe, DC_FB_ORIGIN, 0);
}

u8 ls7a2000_scanout_bank(struct ls7a2000_hw *hw, unsigned int pipe)
{
	return !!(pipe_read(hw, pipe, DC_FB_CONFIG) & FB_BANK);
}

u8 ls7a2000_scanout_flip(struct ls7a2000_hw *hw, unsigned int pipe,
			 u32 bus_address)
{
	u32 config = pipe_read(hw, pipe, DC_FB_CONFIG);
	u8 target = !(config & FB_BANK);

	pipe_write(hw, pipe, target ? DC_FB_ADDRESS1 : DC_FB_ADDRESS0, bus_address);
	/* Publish the new VRAM contents and address before requesting a switch. */
	wmb();
	pipe_write(hw, pipe, DC_FB_CONFIG, (config & ~FB_BANK) | FB_SWITCH);
	return target;
}

bool ls7a2000_scanout_enabled(struct ls7a2000_hw *hw, unsigned int pipe)
{
	return pipe_read(hw, pipe, DC_FB_CONFIG) & FB_ENABLE;
}

void ls7a2000_cursor_show(struct ls7a2000_hw *hw, unsigned int pipe,
			  u32 bus_address, int x, int y)
{
	const struct cursor_registers *regs = &cursor_regs[pipe];
	u32 config, position;

	config = CURSOR_ARGB8888 | CURSOR_64 | (pipe ? CURSOR_ROUTE : 0) |
		 (u32)max(-x, 0) << 16 | (u32)max(-y, 0) << 8;
	position = (u32)max(y, 0) << 16 | (u32)max(x, 0);
	dc_write(hw, regs->config, 0);
	dc_write(hw, regs->address_high, 0);
	dc_write(hw, regs->address, bus_address);
	dc_write(hw, regs->position, position);
	/* Publish the cursor data and address before enabling fetches. */
	wmb();
	dc_write(hw, regs->config, config);
}

void ls7a2000_cursor_hide(struct ls7a2000_hw *hw, unsigned int pipe)
{
	dc_write(hw, cursor_regs[pipe].config, 0);
}

void ls7a2000_cursor_move(struct ls7a2000_hw *hw, unsigned int pipe, int x, int y)
{
	const struct cursor_registers *regs = &cursor_regs[pipe];
	u32 hotspot_mask = GENMASK(21, 16) | GENMASK(13, 8);
	u32 hotspot = (u32)max(-x, 0) << 16 | (u32)max(-y, 0) << 8;
	u32 config = dc_read(hw, regs->config);

	/* Most moves need only one write; the image/address/format stay intact. */
	dc_write(hw, regs->position, (u32)max(y, 0) << 16 | (u32)max(x, 0));
	if ((config & hotspot_mask) != hotspot) {
		/* Write documented fields only; reserved readback bits are not controls. */
		config &= CURSOR_64 | CURSOR_ROUTE | GENMASK(1, 0);
		dc_write(hw, regs->config, config | hotspot);
	}
	/* Complete posted position writes before the immediate commit returns. */
	dc_read(hw, regs->position);
}

u32 ls7a2000_vblank_counter(struct ls7a2000_hw *hw, unsigned int pipe)
{
	return pipe_read(hw, pipe, DC_VSYNC_COUNT);
}

static u16 vsync_mask(unsigned int pipe)
{
	return pipe ? IRQ_VSYNC1 : IRQ_VSYNC0;
}

void ls7a2000_vblank_enable(struct ls7a2000_hw *hw, unsigned int pipe)
{
	unsigned long flags;
	u16 mask = vsync_mask(pipe);

	spin_lock_irqsave(&hw->irq_lock, flags);
	hw->irq_mask |= mask;
	/* Only this pipe's old status is W1C; other pending statuses are untouched. */
	dc_write(hw, DC_INTERRUPT, (u32)hw->irq_mask << 16 | mask);
	spin_unlock_irqrestore(&hw->irq_lock, flags);
}

void ls7a2000_vblank_disable(struct ls7a2000_hw *hw, unsigned int pipe)
{
	unsigned long flags;

	spin_lock_irqsave(&hw->irq_lock, flags);
	hw->irq_mask &= ~vsync_mask(pipe);
	dc_write(hw, DC_INTERRUPT, (u32)hw->irq_mask << 16);
	spin_unlock_irqrestore(&hw->irq_lock, flags);
}

void ls7a2000_irq_disable_all(struct ls7a2000_hw *hw)
{
	unsigned long flags;

	spin_lock_irqsave(&hw->irq_lock, flags);
	hw->irq_mask = 0;
	dc_write(hw, DC_INTERRUPT, U16_MAX);
	spin_unlock_irqrestore(&hw->irq_lock, flags);
}

u32 ls7a2000_irq_read_and_ack(struct ls7a2000_hw *hw)
{
	unsigned long flags;
	u16 pending;
	u32 pipes = 0;

	spin_lock_irqsave(&hw->irq_lock, flags);
	pending = dc_read(hw, DC_INTERRUPT) & hw->irq_mask;
	if (pending)
		dc_write(hw, DC_INTERRUPT, (u32)hw->irq_mask << 16 | pending);
	spin_unlock_irqrestore(&hw->irq_lock, flags);
	if (pending & IRQ_VSYNC0)
		pipes |= BIT(0);
	if (pending & IRQ_VSYNC1)
		pipes |= BIT(1);
	return pipes;
}
