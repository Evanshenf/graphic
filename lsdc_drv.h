/*================================================================
 *   Copyright (C) 2024Ltd. All rights reserved.
 *   
 *   文件名称：lsdc_drv.h
 *   创 建 者：sw
 *   创建日期：2024年05月13日
 *   描    述：
 *
 ================================================================*/


#ifndef LSDC_DRV_H__
#define LSDC_DRV_H__


#include <linux/pci.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/drm_plane.h>
#include <drm/ttm/ttm_device.h>

#include "lsdc_gem.h"
#include "lsdc_regs.h"


struct lsdc_display_pipe;
struct lsdc_gem {
	/* @mutex: protect objects list */
	struct mutex mutex;
	struct list_head objects;
};



struct lsdc_reg32 {
	char *name;
	u32 offset;
};

struct lsdc_crtc;

struct lsdc_crtc_hw_ops {
	void (*enable)(struct lsdc_crtc *lcrtc);
	void (*disable)(struct lsdc_crtc *lcrtc);
	void (*enable_vblank)(struct lsdc_crtc *lcrtc);
	void (*disable_vblank)(struct lsdc_crtc *lcrtc);
	void (*flip)(struct lsdc_crtc *lcrtc);
	void (*clone)(struct lsdc_crtc *lcrtc);
	void (*get_scan_pos)(struct lsdc_crtc *lcrtc, int *hpos, int *vpos);
	void (*set_mode)(struct lsdc_crtc *lcrtc, const struct drm_display_mode *mode);
	void (*soft_reset)(struct lsdc_crtc *lcrtc);
	void (*reset)(struct lsdc_crtc *lcrtc);

	u32  (*get_vblank_counter)(struct lsdc_crtc *lcrtc);
	void (*set_dma_step)(struct lsdc_crtc *lcrtc, enum lsdc_dma_steps step);
};

struct lsdc_crtc {
	struct drm_crtc base;
//	struct lsdc_pixpll pixpll;
	struct drm_lsdc_private *ldev;
	const struct lsdc_crtc_hw_ops *hw_ops;
	const struct lsdc_reg32 *preg;
	unsigned int nreg;
	struct drm_info_list *p_info_list;
	unsigned int n_info_list;
	bool has_vblank;
};

/* primary plane hardware related ops */

struct lsdc_primary;

struct lsdc_primary_plane_ops {
	void (*update_fb_addr)(struct lsdc_primary *plane, u64 addr);
	void (*update_fb_stride)(struct lsdc_primary *plane, u32 stride);
	void (*update_fb_format)(struct lsdc_primary *plane,
			const struct drm_format_info *format);
};


struct lsdc_primary {
	struct drm_plane base;
	const struct lsdc_primary_plane_ops *ops;
	struct drm_lsdc_private *ldev;
};


/* cursor plane hardware related ops */

struct lsdc_cursor;

struct lsdc_cursor_plane_ops {
	void (*update_bo_addr)(struct lsdc_cursor *plane, u64 addr);
	void (*update_cfg)(struct lsdc_cursor *plane,
			enum lsdc_cursor_size cursor_size,
			enum lsdc_cursor_format);
	void (*update_position)(struct lsdc_cursor *plane, int x, int y);
};

struct lsdc_cursor {
	struct drm_plane base;
	const struct lsdc_cursor_plane_ops *ops;
	struct drm_lsdc_private *ldev;
};

struct lsdc_output {
	struct drm_encoder encoder;
	struct drm_connector connector;
};


static inline struct lsdc_output *
connector_to_lsdc_output(struct drm_connector *connector)
{
    return container_of(connector, struct lsdc_output, connector);
}


static inline struct lsdc_output *
encoder_to_lsdc_output(struct drm_encoder *encoder)
{
    return container_of(encoder, struct lsdc_output, encoder);
}

struct lsdc_display_pipe {
	struct lsdc_crtc crtc;
	struct lsdc_primary primary;
	struct lsdc_cursor cursor;
	struct lsdc_output output;
	struct lsdc_i2c *li2c;
	unsigned int index;
};
static inline struct lsdc_display_pipe *
output_to_display_pipe(struct lsdc_output *output)
{
    return container_of(output, struct lsdc_display_pipe, output);
}



struct drm_lsdc_private {
	struct drm_device drm_dev;           // 指向DRM设备结构体的指针
	struct pci_dev *pdev;                 // 指向PCI设备结构体的指针

	// 资源管理
	void __iomem *mmio_base;              // 内存映射I/O基地址
	struct resource *resources;           // 设备资源数组

	// 中断处理
	int irq;                              // 中断号
	spinlock_t irq_lock;                  // 中断锁
	spinlock_t reglock;                  // 中断锁
	u32 irq_status;

	struct tasklet_struct irq_tasklet;    // 中断下半部任务结构体

	// 显示管理
	struct drm_crtc **crtcs;              // CRTC数组
	struct drm_encoder **encoders;        // 编码器数组
	struct drm_connector **connectors;    // 连接器数组

	// 缓冲区管理
	struct lsdc_display_pipe dispipe[2];
	struct lsdc_gem gem;
	struct drm_gem_object **gem_objects;  // GEM对象数组
	struct drm_framebuffer **framebuffers;// 帧缓冲区数组

	// 内存管理
	struct drm_mm mm;                     // DRM内存管理器
	struct ttm_device bdev;

	// 电源管理
	//    struct pm_runtime_data pm_data;       // 电源管理数据
	bool suspended;                       // 设备挂起状态标志

	// 设备特定数据
	uint32_t specific_register;           // 设备特定寄存器
	bool specific_flag;                   // 设备特定标志

	// 可选的设备特定字段，视需求而定
	struct mutex ioregs_lock;             // 保护I/O寄存器的互斥锁
	void __iomem *ioregs;                 // I/O寄存器基地址
	void __iomem *vram;                   // 显存基地址
	unsigned long vram_base;              // 显存基地址
	unsigned long vram_size;              // 显存大小
	unsigned long vram_fb_available;      // 可用帧缓冲显存大小
	    /* @num_output: count the number of active display pipe */
    unsigned int num_output;
	  /* tracking pinned memory */
    size_t vram_pinned_size;
    size_t gtt_pinned_size;



};

static inline struct drm_lsdc_private *tdev_to_ldev(struct ttm_device *bdev)
{
    return container_of(bdev, struct drm_lsdc_private, bdev);
}



static inline struct drm_lsdc_private *to_lsdc(const struct drm_device *dev)
{
	return container_of(dev, struct drm_lsdc_private, drm_dev);
}

static inline struct drm_lsdc_private *kdev_to_lsdc(struct device *kdev)
{
	return dev_get_drvdata(kdev);
}

static inline struct drm_lsdc_private *pdev_to_lsdc(struct pci_dev *pdev)
{
	return pci_get_drvdata(pdev);
}


static inline u32 lsdc_rreg32(struct drm_lsdc_private *ldev, u32 offset)
{
    return readl(ldev->ioregs + offset);
}

static inline void lsdc_wreg32(struct drm_lsdc_private *ldev, u32 offset, u32 val)
{
    writel(val, ldev->ioregs + offset);
}


static inline u32 lsdc_pipe_rreg32(struct drm_lsdc_private *ldev,
                   u32 offset, u32 pipe)
{
    return readl(ldev->ioregs + offset + pipe * CRTC_PIPE_OFFSET);
}

static inline void lsdc_pipe_wreg32(struct drm_lsdc_private *ldev,
                    u32 offset, u32 pipe, u32 val)
{
    writel(val, ldev->ioregs + offset + pipe * CRTC_PIPE_OFFSET);
}



struct lsdc_kms_funcs {
	irqreturn_t (*irq_handler)(int irq, void *arg);

	int (*create_i2c)(struct drm_device *ddev,
			struct lsdc_display_pipe *dispipe,
			unsigned int index);

	int (*output_init)(struct drm_device *ddev,
			struct lsdc_display_pipe *dispipe,
			struct i2c_adapter *ddc,
			unsigned int index);

	int (*cursor_plane_init)(struct drm_device *ddev,
			struct drm_plane *plane,
			unsigned int index);

	int (*primary_plane_init)(struct drm_device *ddev,
			struct drm_plane *plane,
			unsigned int index);

	int (*crtc_init)(struct drm_device *ddev,
			struct drm_crtc *crtc,
			struct drm_plane *primary,
			struct drm_plane *cursor,
			unsigned int index,
			bool has_vblank);
};

static inline struct lsdc_crtc *
to_lsdc_crtc(struct drm_crtc *crtc)
{
    return container_of(crtc, struct lsdc_crtc, base);
}

static inline struct lsdc_display_pipe *
crtc_to_display_pipe(struct drm_crtc *crtc)
{
    return container_of(crtc, struct lsdc_display_pipe, crtc.base);
}

static inline struct lsdc_primary *
to_lsdc_primary(struct drm_plane *plane)
{
    return container_of(plane, struct lsdc_primary, base);
}

static inline struct lsdc_cursor *
to_lsdc_cursor(struct drm_plane *plane)
{
    return container_of(plane, struct lsdc_cursor, base);
}
int ls7a2000_output_init(struct drm_device *ddev,
             struct lsdc_display_pipe *dispipe,
             struct i2c_adapter *ddc,
             unsigned int pipe);

int ls7a2000_cursor_plane_init(struct drm_device *ddev,
                   struct drm_plane *plane,
                   unsigned int index);
int lsdc_primary_plane_init(struct drm_device *ddev,
                struct drm_plane *plane,
                unsigned int index);
#endif //LSDC_DRV_H__

