
#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include "lsdc_driver.h"
#include "lsdc_drv.h"
#include "lsdc_pci.h"

static int loongson_modeset_temp = -1;
static int lsdc_check_nomodeset(void)
{
	bool use_kms = true;

	printk("hello world\n");
	if (drm_firmware_drivers_only() && loongson_modeset_temp == -1)
		use_kms = false;

	if (!use_kms) {

		DRM_DEBUG_DRIVER("KMS disable.\n");
		return 1;
	}
	return 0;
}



static const struct {
	int (*init)(void);
	void (*exit)(void);
} init_funcs[] = {
	{ .init = lsdc_check_nomodeset },
	{ .init = lsdc_pci_register_driver,
	  .exit = lsdc_pci_unregister_driver },
};
static int  init_progress;




static int __init lsdc_init(void)
{
	int err, i;
	for (i =0; i < ARRAY_SIZE(init_funcs); i++) {
		err = init_funcs[i].init();
		if (err < 0) {
			while (i--) {
				if (init_funcs[i].exit)
					init_funcs[i].exit();
			}
			return err;
		} else if (err >0) {
			/*
			 * Early-exit success is reserved for things which
			 * don't have an exit() function because we have no
			 * idea how far they got or how to partially tear
			 * them down.
			 */
			WARN_ON(init_funcs[i].exit);
			break;
		}
	}

	init_progress = i;

	return 0;
}


static void __exit lsdc_exit(void)
{
	int i;

	for (i = init_progress - 1; i >= 0; i--) {
		//GEM_BUG_ON(i >= ARRAY_SIZE(init_funcs));
		if (init_funcs[i].exit)
			init_funcs[i].exit();
	}
}

module_init(lsdc_init);
module_exit(lsdc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("archwse");

MODULE_DESCRIPTION(DRIVER_DESC);

