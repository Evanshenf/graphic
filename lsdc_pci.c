/*================================================================
 *   Copyright (C) 2024Ltd. All rights reserved.
 *   
 *   文件名称：lsdc_pci.c
 *   创 建 者：sw
 *   创建日期：2024年05月13日
 *   描    述：
 *
 ================================================================*/
#include <drm/drm_color_mgmt.h>
#include <drm/drm_drv.h>
#include <drm/drm_aperture.h>
#include <drm/drm_atomic_helper.h>
#include <linux/mod_devicetable.h>
#include <linux/pci.h>

#include "lsdc_driver.h"
#include "lsdc_drv.h"
#include "lsdc_pci.h"
#include "lsdc_reg.h"
#include "loongson_pci_config.h"

static const struct pci_device_id pciidlist[] = {
	{0x0014,0x7a36, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
};

static void lsdc_pci_remove(struct pci_dev *pdev)
{

	struct drm_device *ddev = pci_get_drvdata(pdev);

//	drm_dev_unregister(ddev);
	drm_atomic_helper_shutdown(ddev);


}
static int lsdc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent) 
{
	int err;
	err = lsdc_driver_probe(pdev,ent);
	if(err)
		return err;
	


	return 0;
}

static void lsdc_pci_shutdown(struct pci_dev *pdev)
{
	drm_atomic_helper_shutdown(pci_get_drvdata(pdev));
}
static struct pci_driver lsdc_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = pciidlist,
	.probe = lsdc_pci_probe,
	.remove = lsdc_pci_remove,
	.shutdown = lsdc_pci_shutdown,
	//	//	.driver.pm = &lsdc_pm_ops,
};

int lsdc_pci_register_driver(void)
{
	return pci_register_driver(&lsdc_pci_driver);
}

void lsdc_pci_unregister_driver(void)
{
	pci_unregister_driver(&lsdc_pci_driver);
}
