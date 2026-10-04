/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GRAPHIC_LS7A2000_H
#define GRAPHIC_LS7A2000_H

#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define LS7A2000_PIPE_COUNT 2
#define LS7A2000_MAX_WIDTH 1920
#define LS7A2000_MAX_HEIGHT 1080
#define LS7A2000_STRIDE_ALIGN 256
#define LS7A2000_CURSOR_SIZE 64

/* Physical timing counts, not DRM objects or a PLL programming request. */
struct ls7a2000_timing {
	u32 clock_khz;
	u16 hdisplay;
	u16 hsync_start;
	u16 hsync_end;
	u16 htotal;
	u16 vdisplay;
	u16 vsync_start;
	u16 vsync_end;
	u16 vtotal;
};

/* Read-only firmware snapshot. The backend does not synthesize PHY settings. */
struct ls7a2000_link {
	struct ls7a2000_timing timing;
	u32 panel;
	u32 hdmi;
	u32 phy;
	u32 phy_pll;
	u16 pixel_ref;
	u16 pixel_loop;
	u16 pixel_out;
	bool pll_out_of_spec;
};

struct ls7a2000_hw {
	void __iomem *dc;
	void __iomem *pixel_pll;
	/* Serializes the mixed enable/W1C register and its software mask. */
	spinlock_t irq_lock;
	u16 irq_mask;
};

void ls7a2000_hw_init(struct ls7a2000_hw *hw, void __iomem *dc,
		      void __iomem *pixel_pll);
int ls7a2000_capture_link(struct ls7a2000_hw *hw, unsigned int pipe,
			  struct ls7a2000_link *link);
bool ls7a2000_connected(struct ls7a2000_hw *hw, unsigned int pipe);

/* Process context. Enabling preserves the captured pixel and PHY PLLs. */
int ls7a2000_pipe_enable(struct ls7a2000_hw *hw, unsigned int pipe,
			 const struct ls7a2000_link *link);
void ls7a2000_pipe_disable(struct ls7a2000_hw *hw, unsigned int pipe,
			   const struct ls7a2000_timing *timing);
void ls7a2000_scanout_prepare(struct ls7a2000_hw *hw, unsigned int pipe,
			      u32 bus_address, u32 pitch);
u8 ls7a2000_scanout_flip(struct ls7a2000_hw *hw, unsigned int pipe,
			 u32 bus_address);
bool ls7a2000_scanout_enabled(struct ls7a2000_hw *hw, unsigned int pipe);
u8 ls7a2000_scanout_bank(struct ls7a2000_hw *hw, unsigned int pipe);

void ls7a2000_cursor_show(struct ls7a2000_hw *hw, unsigned int pipe,
			  u32 bus_address, int x, int y);
/* The current image remains pinned; only location/hotspot may change. */
void ls7a2000_cursor_move(struct ls7a2000_hw *hw, unsigned int pipe, int x, int y);
void ls7a2000_cursor_hide(struct ls7a2000_hw *hw, unsigned int pipe);
u32 ls7a2000_vblank_counter(struct ls7a2000_hw *hw, unsigned int pipe);
void ls7a2000_vblank_enable(struct ls7a2000_hw *hw, unsigned int pipe);
void ls7a2000_vblank_disable(struct ls7a2000_hw *hw, unsigned int pipe);
void ls7a2000_irq_disable_all(struct ls7a2000_hw *hw);
/* Returns BIT(pipe) for each acknowledged VSYNC, hiding hardware bit numbers. */
u32 ls7a2000_irq_read_and_ack(struct ls7a2000_hw *hw);

#endif
