/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LSDC_DISPLAY_DRIVER_H
#define LSDC_DISPLAY_DRIVER_H

struct drm_crtc;
struct drm_lsdc_private;
struct lsdc_display_pipe;

int lsdc_display_driver_init(struct drm_lsdc_private *dev_priv);
int lsdc_plane_init(struct lsdc_display_pipe *pipe);
int lsdc_crtc_init(struct lsdc_display_pipe *pipe);
void lsdc_crtc_abort_pending(struct drm_crtc *crtc);
int lsdc_irq_init(struct drm_lsdc_private *dev_priv);
void lsdc_irq_fini(struct drm_lsdc_private *dev_priv);

#endif
