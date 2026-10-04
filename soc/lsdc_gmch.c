// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */

#include <linux/aperture.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/pci.h>

#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "lsdc_driver.h"
#include "lsdc_drv.h"
#include "soc/lsdc_gmch.h"

#define LSDC_GPU_VRAM_BAR 2
#define LSDC_PIX_PLL_OFFSET 0x04b0
#define LSDC_PIX_PLL_SIZE 0x20

/*
 * Manual V1.3, sections 3.1.3 and 4: this is a BIOS-selected bridge
 * configuration window, not the DC BAR or PCI configuration space.
 * The default is the lab board's firmware mapping, not a PCI discovery rule.
 */
static unsigned long bridge_base = 0x10010000;
module_param(bridge_base, ulong, 0400);
MODULE_PARM_DESC(bridge_base,
		 "BIOS-dependent bridge configuration physical base (lab: 0x10010000)");

static void lsdc_gmch_release_vram(struct drm_device *drm, void *data)
{
	struct pci_dev *gpu = data;

	pci_release_region(gpu, LSDC_GPU_VRAM_BAR);
	pci_dev_put(gpu);
}

static int lsdc_gmch_reserve_vram(struct drm_lsdc_private *dev_priv)
{
	struct pci_dev *dc = dev_priv->pdev;
	struct drm_device *drm = &dev_priv->drm_dev;
	struct pci_dev *gpu;
	u64 bus_end, cpu_end;
	int ret;

	/* Manual V1.3, sections 3.1.1 and 19.1: GPU D6:F0, DC D6:F1. */
	gpu = pci_get_domain_bus_and_slot(pci_domain_nr(dc->bus),
					  dc->bus->number, PCI_DEVFN(6, 0));
	if (!gpu)
		return -ENODEV;

	device_lock(&gpu->dev);
	if (gpu->vendor != 0x0014 || gpu->device != 0x7a25) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* Never displace a bound GPU driver to obtain the shared VRAM. */
	if (gpu->dev.driver) {
		drm_err(drm, "Paired GPU is bound; shared VRAM is unavailable\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	if (!(pci_resource_flags(gpu, LSDC_GPU_VRAM_BAR) & IORESOURCE_MEM)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	dev_priv->vram_base = pci_resource_start(gpu, LSDC_GPU_VRAM_BAR);
	dev_priv->vram_size = pci_resource_len(gpu, LSDC_GPU_VRAM_BAR);
	dev_priv->vram_bus_base = pci_bus_address(gpu, LSDC_GPU_VRAM_BAR);
	if (dev_priv->vram_size < PAGE_SIZE ||
	    !IS_ALIGNED(dev_priv->vram_base, PAGE_SIZE) ||
	    !IS_ALIGNED(dev_priv->vram_bus_base, PAGE_SIZE) ||
	    (pci_resource_flags(gpu, LSDC_GPU_VRAM_BAR) & IORESOURCE_UNSET) ||
	    check_add_overflow((u64)dev_priv->vram_base,
			       (u64)dev_priv->vram_size - 1, &cpu_end) ||
	    check_add_overflow(dev_priv->vram_bus_base,
			       (u64)dev_priv->vram_size - 1, &bus_end) ||
	    bus_end > U32_MAX) {
		drm_err(drm, "VRAM is outside the documented 32-bit scanout range\n");
		ret = -ERANGE;
		goto out_unlock;
	}

	/*
	 * Firmware DRM drivers can hold a busy subregion of this BAR. Their
	 * aperture teardown releases that reservation before we claim the BAR.
	 * Keep the GPU device locked so no native GPU driver can bind midway.
	 */
	ret = aperture_remove_conflicting_devices(dev_priv->vram_base,
						  dev_priv->vram_size,
						  DRIVER_NAME);
	if (ret)
		goto out_unlock;

	ret = pci_request_region(gpu, LSDC_GPU_VRAM_BAR, DRIVER_NAME);
out_unlock:
	device_unlock(&gpu->dev);
	if (ret) {
		pci_dev_put(gpu);
		return ret;
	}

	/* Keep the VRAM reservation until the final DRM/BO references are gone. */
	ret = drmm_add_action_or_reset(drm, lsdc_gmch_release_vram, gpu);
	if (ret)
		return ret;

	drm_info(drm, "VRAM: CPU %pa, PCI bus %#llx, size %pa\n",
		 &dev_priv->vram_base, dev_priv->vram_bus_base,
		 &dev_priv->vram_size);
	return 0;
}

int lsdc_gmch_init(struct drm_lsdc_private *dev_priv)
{
	resource_size_t pll_base, pll_end;
	int ret;

	if (!bridge_base || !IS_ALIGNED(bridge_base, sizeof(u32)) ||
	    check_add_overflow((resource_size_t)bridge_base,
			       (resource_size_t)LSDC_PIX_PLL_OFFSET, &pll_base) ||
	    check_add_overflow(pll_base,
			       (resource_size_t)LSDC_PIX_PLL_SIZE - 1, &pll_end))
		return -EINVAL;

	dev_priv->pll_regs = devm_ioremap(&dev_priv->pdev->dev, pll_base,
					  LSDC_PIX_PLL_SIZE);
	if (!dev_priv->pll_regs)
		return -ENOMEM;

	ret = lsdc_gmch_reserve_vram(dev_priv);
	if (ret)
		return ret;

	drm_info(&dev_priv->drm_dev,
		 "Using board firmware bridge base %#lx for PIX PLLs\n",
		 bridge_base);
	return 0;
}
