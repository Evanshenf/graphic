/*================================================================
 *   Copyright (C) 2024Ltd. All rights reserved.
 *   
 *   文件名称：lsdc_display_driver.c
 *   创 建 者：sw
 *   创建日期：2024年05月15日
 *   描    述：
 *
 ================================================================*/

#include <linux/vga_switcheroo.h>
#include <acpi/video.h>
#include <drm/display/drm_dp_mst_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_privacy_screen_consumer.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <linux/pci.h>
#include <linux/vgaarb.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "lsdc_drv.h"
#include "lsdc_display_driver.h"
#include "lsdc_vga.h"
extern struct lsdc_kms_funcs ls7a2000_kms_funcs;


static const struct drm_mode_config_funcs lsdc_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs lsdc_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail,
	//.atomic_commit_setup = drm_dp_mst_atomic_setup_commit,
};




static void lsdc_mode_config_init(struct drm_lsdc_private *lsdc) 
{
	struct drm_mode_config *mode_config = &lsdc->drm_dev.mode_config;
	drm_mode_config_init(&lsdc->drm_dev);

	// 配置支持的最小和最大分辨率
	mode_config->min_width = 1;
	mode_config->min_height = 1;
	mode_config->max_width = 8192;
	mode_config->max_height = 8192;
	// 设置首选颜色深度和阴影缓冲
	mode_config->preferred_depth = 24;
	mode_config->prefer_shadow = 1;

	// 配置光标尺寸
	mode_config->cursor_width = 64;
	mode_config->cursor_height = 64;
	// 设置模式设置回调函数
	mode_config->funcs = &lsdc_mode_config_funcs;
	// 设置模式配置助手函数
	mode_config->helper_private = &lsdc_mode_config_helper_funcs;
}

int lsdc_display_driver_probe_noirq (struct drm_lsdc_private *lsdc)
{
	int ret = 0;

	ret = drm_vblank_init(&lsdc->drm_dev, 2);
	if (ret)
		return ret;
	ret = lsdc_vga_register(lsdc);
	if (ret)
		return ret;

	lsdc_mode_config_init(lsdc);
//	ret = lsdc_modeset_init(lsdc, 2,  NULL, 1);
	if (ret)
		return ret;

	drm_mode_config_reset(&lsdc->drm_dev);

	return 0;
}
void lsdc_display_driver_register(struct drm_lsdc_private *lsdc)
{
	/*
	 * We need to coordinate the hotplugs with the asynchronous
	 * fbdev configuration, for which we use the
	 * fbdev->async_cookie.
	 */
	drm_kms_helper_poll_init(&lsdc->drm_dev);

}



