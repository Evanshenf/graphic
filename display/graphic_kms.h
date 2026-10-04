/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GRAPHIC_KMS_H
#define GRAPHIC_KMS_H

struct graphic_device;

/* Failed init unwinds its active IRQ/polling; DRM manages partial objects. */
int graphic_kms_init(struct graphic_device *gdev);
void graphic_kms_stop(struct graphic_device *gdev);

#endif
