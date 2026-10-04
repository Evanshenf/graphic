/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LSDC_OUTPUT_H
#define LSDC_OUTPUT_H

struct drm_display_mode;
struct lsdc_display_pipe;

/* Capture the configured mode, including DPMS-off, before hardware writes. */
int lsdc_output_init(struct lsdc_display_pipe *pipe);
/* Enable only the captured mode, retaining both pixel and PHY PLL settings. */
int lsdc_output_enable(struct lsdc_display_pipe *pipe,
		       const struct drm_display_mode *mode);
void lsdc_output_disable(struct lsdc_display_pipe *pipe);

#endif
