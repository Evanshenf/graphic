// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>

#include "display/lsdc_display_driver.h"
#include "lsdc_driver.h"
#include "lsdc_drv.h"
#include "lsdc_gem.h"
#include "lsdc_region_ttm.h"
#include "soc/lsdc_gmch.h"

DEFINE_DRM_GEM_FOPS(lsdc_gem_fops);

static const struct drm_driver lsdc_kms_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &lsdc_gem_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = 1,
	.minor = 1,
	.dumb_create = lsdc_gem_dumb_create,
	.dumb_map_offset = drm_gem_ttm_dumb_map_offset,
	DRM_FBDEV_TTM_DRIVER_OPS,
};

int lsdc_driver_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct drm_lsdc_private *dev_priv;
	struct drm_device *drm;
	void __iomem *ioregs;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) < SZ_64K)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "DC BAR0 must cover the 64 KiB register window\n");

	ioregs = pcim_iomap_region(pdev, 0, DRIVER_NAME);
	if (IS_ERR(ioregs))
		return PTR_ERR(ioregs);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	dev_priv = devm_drm_dev_alloc(&pdev->dev, &lsdc_kms_driver,
				      struct drm_lsdc_private, drm_dev);
	if (IS_ERR(dev_priv))
		return PTR_ERR(dev_priv);

	dev_priv->pdev = pdev;
	dev_priv->ioregs = ioregs;
	spin_lock_init(&dev_priv->irq_lock);
	drm = &dev_priv->drm_dev;
	pci_set_drvdata(pdev, dev_priv);

	ret = lsdc_gmch_init(dev_priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Cannot claim paired GPU VRAM and PIX PLL window\n");

	lsdc_gem_init(drm);
	ret = lsdc_ttm_init(dev_priv);
	if (ret)
		return ret;

	/* Display initialization captures firmware state before changing HW. */
	ret = lsdc_display_driver_init(dev_priv);
	if (ret)
		return ret;

	ret = lsdc_irq_init(dev_priv);
	if (ret)
		goto err_shutdown;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_irq;

	/* XRGB8888 selects 32-bit storage while keeping the alpha byte unused. */
	drm_client_setup(drm, drm_format_info(DRM_FORMAT_XRGB8888));
	return 0;

err_irq:
	drm_kms_helper_poll_disable(drm);
	drm_atomic_helper_shutdown(drm);
	lsdc_irq_fini(dev_priv);
	return ret;

err_shutdown:
	drm_kms_helper_poll_disable(drm);
	drm_atomic_helper_shutdown(drm);
	return ret;
}

void lsdc_driver_remove(struct pci_dev *pdev)
{
	struct drm_lsdc_private *dev_priv = pci_get_drvdata(pdev);
	struct drm_device *drm = &dev_priv->drm_dev;

	drm_dev_unregister(drm);
	drm_kms_helper_poll_disable(drm);
	drm_atomic_helper_shutdown(drm);
	lsdc_irq_fini(dev_priv);
}

void lsdc_driver_shutdown(struct pci_dev *pdev)
{
	struct drm_lsdc_private *dev_priv = pci_get_drvdata(pdev);

	drm_kms_helper_poll_disable(&dev_priv->drm_dev);
	drm_atomic_helper_shutdown(&dev_priv->drm_dev);
	lsdc_irq_fini(dev_priv);
}
