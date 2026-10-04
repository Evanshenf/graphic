/*================================================================
 *   Copyright (C) 2024Ltd. All rights reserved.
 *   
 *   文件名称：lsdc_driver.c
 *   创 建 者：sw
 *   创建日期：2024年05月13日
 *   描    述：
 *
 ================================================================*/
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/oom.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>
#include <linux/vga_switcheroo.h>
#include <linux/vt.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>

#include <drm/drm_fbdev_generic.h>
#include <linux/console.h>

#include <drm/drm.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_drv.h>

#include "lsdc_driver.h"
#include "lsdc_drv.h"
#include "lsdc_region_ttm.h"
#include "display/lsdc_display_driver.h"

static const struct drm_driver lsdc_kms_driver ;

static void lsdc_driver_register(struct drm_lsdc_private *dev_priv)
{
	/* Reveal our presence to userspace */
	if (drm_dev_register(&dev_priv->drm_dev, 0)) {
		drm_err(&dev_priv->drm_dev,
				"Failed to register driver for userspace access!\n");
		return;
	}

	lsdc_display_driver_register(dev_priv);



}
static int lsdc_driver_early_probe(struct drm_lsdc_private *dev_priv)
{
	int ret = 0;
	ret = lsdc_ttm_init(dev_priv);
	if (ret)
		goto err_ttm;

	lsdc_gem_init(&dev_priv->drm_dev);

err_ttm:
	return ret;

}
	static struct drm_lsdc_private *
lsdc_driver_create(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct drm_lsdc_private *lsdc;
	struct drm_device *dev;
	struct pci_dev *pdev_gpu;
	int ret = 0;
	lsdc = devm_drm_dev_alloc(&pdev->dev, &lsdc_kms_driver,
			struct drm_lsdc_private, drm_dev);
	if (IS_ERR(lsdc))
		return lsdc;
	dev = &lsdc->drm_dev;
	pci_set_drvdata(pdev, lsdc);

	pdev_gpu = pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus),pdev->bus->number,PCI_DEVFN(6,0));
	if (!pdev_gpu) {
		drm_err(dev, "NO gpu device, then no VRAM\n");
		return ERR_PTR(-EIO);
	}
	lsdc->vram_base = pci_resource_start(pdev_gpu, 2);
	lsdc->vram_size = pci_resource_len(pdev_gpu, 2);

	drm_info(dev, "Dedicated vram start: 0x%llx, size: %uMiB\n",
			(u64)lsdc->vram_base, (u32)(lsdc->vram_size >> 20));

	ret = drm_aperture_remove_conflicting_pci_framebuffers(pdev, &lsdc_kms_driver);
	ret = drmm_mutex_init(dev, &lsdc->ioregs_lock);
	if (ret)
		return ERR_PTR(ret);
	lsdc->ioregs = pcim_iomap(pdev, 0, 0); 
	if (!lsdc->ioregs)
		return ERR_PTR(-EIO);

	return lsdc;
}


int lsdc_driver_probe(struct pci_dev *pdev,const struct pci_device_id *ent)
{
	struct drm_lsdc_private *lsdc;
	struct drm_device *drm_dev;
	int ret;
	ret = pci_enable_device(pdev);
	if (ret) {
		pr_err("Failed to enable graphics device: %pe\n", ERR_PTR(ret));
		return ret;
	}
	lsdc = lsdc_driver_create(pdev, ent);
	if (IS_ERR(lsdc)) {
		pci_disable_device(pdev);
		return PTR_ERR(lsdc);
	}
	lsdc_driver_early_probe(lsdc);
	if (ret < 0)
		goto out_pci_disable;
	drm_dev = &lsdc->drm_dev;

	ret = lsdc_display_driver_probe_noirq(lsdc);

	lsdc_driver_register(lsdc);


	drm_fbdev_generic_setup(drm_dev, 32);
	return 0;
out_pci_disable:
	pci_disable_device(pdev);
	drm_err(drm_dev, "Device initialization failed (%d)\n", ret);
	return ret;
}










static const struct file_operations lsdc_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release_noglobal,
	.unlocked_ioctl = drm_ioctl,
	.mmap = drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.compat_ioctl = drm_compat_ioctl,
	.llseek = noop_llseek,
#ifdef CONFIG_PROC_FS
	.show_fdinfo = drm_show_fdinfo,
#endif
};

#define DRIVER_MAJOR		1
#define DRIVER_MINOR		1
#define DRIVER_PATCHLEVEL	0
static const struct drm_driver lsdc_kms_driver = {

	.driver_features = DRIVER_GEM |
		DRIVER_RENDER| DRIVER_MODESET | DRIVER_ATOMIC,

	.fops = &lsdc_driver_fops,
	.major = DRIVER_MAJOR,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,


};
