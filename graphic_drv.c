// SPDX-License-Identifier: GPL-2.0
/*
 * Device/resource lifetime reorganized from graphic commit 18c1161.
 * Retained code attribution:
 * Copyright (C) 2024Ltd. All rights reserved.
 * Original author: sw
 */
#include <linux/aperture.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "display/graphic_kms.h"
#include "graphic_device.h"
#include "memory/graphic_bo.h"

#define GRAPHIC_NAME "graphic"
#define GRAPHIC_GPU_VRAM_BAR 2
#define GRAPHIC_PIXEL_PLL_OFFSET 0x04b0
#define GRAPHIC_PIXEL_PLL_SIZE 0x20

/* Manual V1.3 sections 3.1.3/4: BIOS chooses this separate bridge window. */
static unsigned long bridge_base = 0x10010000;
module_param(bridge_base, ulong, 0400);
MODULE_PARM_DESC(bridge_base,
		 "BIOS-dependent bridge configuration physical base (lab: 0x10010000)");

DEFINE_DRM_GEM_FOPS(graphic_fops);

static const struct drm_driver graphic_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &graphic_fops,
	.name = GRAPHIC_NAME,
	.desc = "Manual-based LS7A2000 display controller",
	.major = 1,
	.minor = 2,
	.dumb_create = graphic_dumb_create,
	.dumb_map_offset = drm_gem_ttm_dumb_map_offset,
	DRM_FBDEV_TTM_DRIVER_OPS,
};

static void graphic_release_vram(struct drm_device *drm, void *data)
{
	struct pci_dev *gpu = data;

	pci_release_region(gpu, GRAPHIC_GPU_VRAM_BAR);
	pci_dev_put(gpu);
}

static int graphic_claim_vram(struct graphic_device *gdev)
{
	struct pci_dev *dc = gdev->pci;
	struct graphic_vram *vram = &gdev->vram;
	struct pci_dev *gpu;
	u64 cpu_end, bus_end;
	int ret;

	/* Manual V1.3 sections 3.1.1/19.1: the paired GPU is D6:F0. */
	gpu = pci_get_domain_bus_and_slot(pci_domain_nr(dc->bus),
					  dc->bus->number, PCI_DEVFN(6, 0));
	if (!gpu)
		return -ENODEV;

	device_lock(&gpu->dev);
	if (gpu->vendor != 0x0014 || gpu->device != 0x7a25) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (gpu->dev.driver) {
		drm_err(&gdev->drm, "Paired GPU is bound; shared VRAM is unavailable\n");
		ret = -EBUSY;
		goto out_unlock;
	}
	if (!(pci_resource_flags(gpu, GRAPHIC_GPU_VRAM_BAR) & IORESOURCE_MEM) ||
	    (pci_resource_flags(gpu, GRAPHIC_GPU_VRAM_BAR) & IORESOURCE_UNSET)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	vram->cpu_start = pci_resource_start(gpu, GRAPHIC_GPU_VRAM_BAR);
	vram->size = pci_resource_len(gpu, GRAPHIC_GPU_VRAM_BAR);
	vram->bus_start = pci_bus_address(gpu, GRAPHIC_GPU_VRAM_BAR);
	if (vram->size < PAGE_SIZE ||
	    !IS_ALIGNED(vram->cpu_start, PAGE_SIZE) ||
	    !IS_ALIGNED(vram->bus_start, PAGE_SIZE) ||
	    check_add_overflow((u64)vram->cpu_start, (u64)vram->size - 1,
			       &cpu_end) ||
	    check_add_overflow(vram->bus_start, (u64)vram->size - 1,
			       &bus_end) || bus_end > U32_MAX) {
		ret = -ERANGE;
		goto out_unlock;
	}

	/*
	 * EFI DRM can reserve a busy subregion of BAR2. Remove its aperture
	 * before claiming the BAR, while the GPU lock excludes a native bind.
	 */
	ret = aperture_remove_conflicting_devices(vram->cpu_start, vram->size,
						  GRAPHIC_NAME);
	if (ret)
		goto out_unlock;
	ret = pci_request_region(gpu, GRAPHIC_GPU_VRAM_BAR, GRAPHIC_NAME);
out_unlock:
	device_unlock(&gpu->dev);
	if (ret) {
		pci_dev_put(gpu);
		return ret;
	}

	/* Added before memory/KMS actions: this lease is released after them. */
	ret = drmm_add_action_or_reset(&gdev->drm, graphic_release_vram, gpu);
	if (ret)
		return ret;

	drm_info(&gdev->drm, "VRAM: CPU %pa, PCI bus %#llx, size %pa\n",
		 &vram->cpu_start, vram->bus_start, &vram->size);
	return 0;
}

static int graphic_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	struct graphic_device *gdev;
	void __iomem *dc, *pixel_pll;
	resource_size_t pll_base, pll_end;
	int ret;

	if (drm_firmware_drivers_only())
		return -ENODEV;
	if (!bridge_base || !IS_ALIGNED(bridge_base, sizeof(u32)) ||
	    check_add_overflow((resource_size_t)bridge_base,
			       (resource_size_t)GRAPHIC_PIXEL_PLL_OFFSET, &pll_base) ||
	    check_add_overflow(pll_base,
			       (resource_size_t)GRAPHIC_PIXEL_PLL_SIZE - 1, &pll_end))
		return -EINVAL;

	ret = pcim_enable_device(pci);
	if (ret)
		return ret;
	if (!(pci_resource_flags(pci, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pci, 0) < SZ_64K)
		return dev_err_probe(&pci->dev, -ENODEV,
				     "DC BAR0 must cover the 64 KiB register window\n");

	dc = pcim_iomap_region(pci, 0, GRAPHIC_NAME);
	if (IS_ERR(dc))
		return PTR_ERR(dc);
	pixel_pll = devm_ioremap(&pci->dev, pll_base, GRAPHIC_PIXEL_PLL_SIZE);
	if (!pixel_pll)
		return -ENOMEM;
	ret = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	gdev = devm_drm_dev_alloc(&pci->dev, &graphic_drm_driver,
				  struct graphic_device, drm);
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);
	gdev->pci = pci;
	pci_set_drvdata(pci, gdev);
	ls7a2000_hw_init(&gdev->hw, dc, pixel_pll);

	ret = graphic_claim_vram(gdev);
	if (ret)
		return dev_err_probe(&pci->dev, ret, "Cannot claim paired GPU VRAM\n");
	ret = graphic_memory_init(gdev);
	if (ret)
		return ret;
	ret = graphic_kms_init(gdev);
	if (ret)
		return ret;

	ret = drm_dev_register(&gdev->drm, 0);
	if (ret) {
		graphic_kms_stop(gdev);
		return ret;
	}
	drm_client_setup(&gdev->drm, drm_format_info(DRM_FORMAT_XRGB8888));
	return 0;
}

static void graphic_remove(struct pci_dev *pci)
{
	struct graphic_device *gdev = pci_get_drvdata(pci);

	drm_dev_unregister(&gdev->drm);
	graphic_kms_stop(gdev);
}

static void graphic_shutdown(struct pci_dev *pci)
{
	graphic_kms_stop(pci_get_drvdata(pci));
}

static const struct pci_device_id graphic_pci_ids[] = {
	{ PCI_DEVICE(0x0014, 0x7a36) },
	{ }
};
MODULE_DEVICE_TABLE(pci, graphic_pci_ids);

static struct pci_driver graphic_pci_driver = {
	.name = GRAPHIC_NAME,
	.id_table = graphic_pci_ids,
	.probe = graphic_probe,
	.remove = graphic_remove,
	.shutdown = graphic_shutdown,
};
module_pci_driver(graphic_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("archwse");
MODULE_DESCRIPTION("Manual-based LS7A2000 display controller");
