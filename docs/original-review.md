# 用户原版 graphic：无法形成显示输出的源码诊断

审查日期：2026-10-04。

基线：`a483dd089dfe5520fafb786568427364601f373d`（`Import the user graphic project before the display rewrite`）。只读工作树：`/home/arch/loongson-gpu-lab/work/graphic-original-audit`。本轮未修改该工作树，没有执行原版模块，没有读取其他 Loongson/gsgpu/lg110 硬件驱动实现。

## 直接结论

**原版是已经搭出 PCI/DRM、display、GEM/TTM、soc 分层的实验框架，尚未实现从 framebuffer 到显示输出的完整链路。首要原因是 KMS 对象和硬件编程都没有接通，不是一个孤立的 PLL 参数或分辨率数值写错。**

显示初始化中的 `lsdc_modeset_init()` 调用已被注释；原版所有 `.c` 文件中没有建立 plane、CRTC、encoder、connector 的调用，也没有调用寄存器写 helper 来编程显示地址、stride、时序或使能。GEM 层只初始化链表/锁，尚未提供 BO 创建及 dumb-buffer 接口。因此，即使模块初始化成功、PCI probe 返回 0，乃至出现 DRM 节点，也不足以让这个版本接管并输出一幅新图像。

这是固定源码可以直接证明的功能缺口。**本轮没有把源码审查冒充为历史故障日志分析或原版实机复现，也没有认定某一次过去的黑屏必然发生在哪条错误分支。**

## 1. 显示链条具体断在哪里

| 环节 | 原版证据 | 实际含义 |
| --- | --- | --- |
| DRM 外壳 | [lsdc_driver.c:78](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:78) 分配 DRM 对象；`:47` 调用 `drm_dev_register()` | 设备注册只是框架入口，不自动创建显示管线或编程硬件 |
| KMS 配置容器 | [display/lsdc_display_driver.c:53](/home/arch/loongson-gpu-lab/work/graphic-original-audit/display/lsdc_display_driver.c:53) 设置 mode_config、范围和 generic atomic helpers | 设置范围和回调不等于创建 CRTC、plane 或输出端口 |
| KMS 管线创建 | [display/lsdc_display_driver.c:88](/home/arch/loongson-gpu-lab/work/graphic-original-audit/display/lsdc_display_driver.c:88) 的 `lsdc_modeset_init(...)` 整行被注释；原版没有其函数实现 | 没有可绑定 framebuffer 的活动显示管线；单纯取消注释也无法补齐实现 |
| plane/CRTC/output | [lsdc_drv.h:48](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_drv.h:48)、`:80`、`:99`、`:252` 只有结构、函数指针和声明；所有 `.c` 未注册实际对象 | 原子检查/提交的通用 helper 没有该设备的硬件回调可执行 |
| 未链接的设备描述 | [loongson_device.c:16](/home/arch/loongson-gpu-lab/work/graphic-original-audit/loongson_device.c:16) 包含不存在的 `lsdc_irq.h`，`:20` 的表引用未提供的函数，`:26` 的 CRTC callback 也被注释；[Makefile:6](/home/arch/loongson-gpu-lab/work/graphic-original-audit/Makefile:6) 至 `:27` 未链接此文件 | 该表不是当前模块实际生效的显示实现；也不能直接把这个文件加回链接就得到完整驱动 |
| 寄存器编程 | [lsdc_drv.h:227](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_drv.h:227) 至 `:247` 有 read/write inline helpers，但所有 `.c` 没有调用这些写 helper，也没有直接 `writel()` | 未形成 framebuffer 地址、stride、时钟、时序和输出使能的硬件编程序列 |
| 中断/vblank | [display/lsdc_display_driver.c:80](/home/arch/loongson-gpu-lab/work/graphic-original-audit/display/lsdc_display_driver.c:80) 仅分配两个 vblank 的框架状态；所有 `.c` 无 `request_irq()`、中断处理实现或 `drm_crtc_handle_vblank()` | `drm_vblank_init()` 不会替硬件申请中断或产生帧完成事件 |

通用 DRM `drm_mode_config_init()` 初始化的是空的对象链表和计数；`drm_mode_config_reset()` 只遍历已经建立的对象，并不会创建它们。由此可解释为何调用这些 helper 仍不能补齐上述缺口。参考：[通用 DRM mode_config 核心](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/drm_mode_config.c)。

## 2. framebuffer/GEM/TTM 层也尚未闭合

- [lsdc_gem.c:21](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_gem.c:21) 是该文件唯一函数，仅初始化 `gem.mutex` 和 `gem.objects`；没有分配 GEM/TTM BO、建立用户 handle、映射或对象析构。
- [lsdc_driver.c:168](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:168) 的 `drm_driver` 没有 `.dumb_create`。Linux 6.7 通用 `drm_mode_create_dumb()` 对这一情况明确返回 `-ENOSYS`。因此，仅调用 `drm_fbdev_generic_setup(drm_dev, 32)` 不会自动生成本驱动缺失的缓冲区分配实现。[6.7 通用 dumb-buffer 核心](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/drm_dumb_buffers.c#L60)
- [lsdc_region_ttm.c:46](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_region_ttm.c:46) 仅调用 `ttm_device_init()`，没有把 GPU BAR2 对应显存注册为 TTM VRAM range manager，也没有相应 teardown。
- [gem/lsdc_gem_ttm.c:27](/home/arch/loongson-gpu-lab/work/graphic-original-audit/gem/lsdc_gem_ttm.c:27) 的已列 TTM callbacks 全部为 NULL，其余未初始化字段也为零。不是说每一个 TTM callback 都强制非空，而是这里尚无可支撑本设备显存 BO 创建、迁移、映射和销毁的完整实现。
- [lsdc_region_ttm.c:109](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_region_ttm.c:109) 有 pin/placement 工具函数，但全树没有 BO 构造及调用闭环。辅助函数本身存在不代表用户态能取得可扫描的 framebuffer。
- [lsdc_region_ttm.c:43](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_region_ttm.c:43) 只返回页偏移；[lsdc_driver.c:90](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:90) 仅取得 CPU resource 地址，没有单独建立 PCI bus/scanout 地址契约。因为原版根本没有执行显示地址写入，本轮不把它夸大成已发生的错误硬件地址写入；它是补全扫描输出时必须解决的接口缺口。

## 3. 与功能缺口独立的错误处理和生命周期问题

| 问题 | 精确位置 | 结论边界 |
| --- | --- | --- |
| TTM/GEM early_probe 返回值丢失 | [lsdc_driver.c:123](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:123) 调用后没有 `ret =`，`:124` 检查的仍是此前成功的 PCI enable 返回值 | early_probe 失败会继续初始化，不会进入所写的失败分支 |
| 显示初始化返回值未检查 | [lsdc_driver.c:128](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:128) 赋值后直接注册设备，`:134` 固定返回 0 | vblank/VGA 等初始化失败可以被伪装成 probe 成功 |
| DRM 注册错误未上送 | [lsdc_driver.c:44](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:44) 是 `void`；`:47` 失败只打印并 return | 外层 probe 仍会调用 fbdev setup 并返回成功 |
| 冲突 framebuffer 移除错误被覆盖 | [lsdc_driver.c:96](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:96) 返回值在下一行被 mutex init 返回值覆盖 | 不能知道显示资源接管是否成功；且传入的是 DC PCI 功能，不能仅据此保证另一 GPU 功能的 VRAM aperture 已被正确处理 |
| mode_config 初始化错误丢失 | [display/lsdc_display_driver.c:56](/home/arch/loongson-gpu-lab/work/graphic-original-audit/display/lsdc_display_driver.c:56) 不检查返回值，包装函数为 `void` | 分配属性等失败后仍继续 reset/register；本轮未注入内存分配失败 |
| PCI ID 表无全零终止项 | [lsdc_pci.c:23](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_pci.c:23) 只有一个匹配项，没有 `{ }`；也未导出 `MODULE_DEVICE_TABLE(pci, ...)` | PCI 核心遍历未匹配项时缺少合法结束标志；缺少自动加载 modalias。未声称已观察到某次越界崩溃 |
| 配对 GPU 引用泄漏 | [lsdc_driver.c:85](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:85) 获取引用，原版全树没有对应 `pci_dev_put()` | 成功和后续失败路径都不释放；通用 PCI lookup 契约明确要求调用者 put |
| GPU/VRAM 占用仲裁缺失 | [lsdc_driver.c:85](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_driver.c:85) 至 `:100` 未校验配对 GPU VID/DID、绑定状态、BAR 类型/大小及占用 | 与另一显存使用者并存时无明确所有权；本机保留 gsgpu blacklist 并不能替代代码中的资源检查 |
| 资源拆除不完整 | [lsdc_pci.c:32](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_pci.c:32) 的 unregister 被注释；全树未调用 poll_fini、mode_config_cleanup、ttm_device_fini；正常 remove 也未配对 PCI disable | `devm_drm_dev_alloc()` 管理 DRM 对象引用，不会替代所有显式初始化接口的清理；后续补全显示对象后，挂起工作和资源寿命问题会更明显 |
| VGA unregister 定义但未调用 | [display/lsdc_vga.c:35](/home/arch/loongson-gpu-lab/work/graphic-original-audit/display/lsdc_vga.c:35) 只有函数定义 | 已注册时没有失败回滚和 remove 对应清理 |
| nomodeset 情况可“加载成功但未注册 PCI 驱动” | [lsdc_module.c:14](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_module.c:14) 返回正值；`:51` 退出初始化循环；`:65` 仍返回 0 | 这是原代码显式设计的 early-exit success，不能把 insmod 返回 0 当作已绑定硬件 |

PCI 引用契约：[Linux 6.7 pci_get_domain_bus_and_slot](https://github.com/torvalds/linux/blob/v6.7/drivers/pci/search.c#L214)。

还有两个容易误报的细节应保留边界：

1. `lsdc_driver.c:137` 的错误日志使用尚未赋值的 `drm_dev`，但当前 `:123` 没保存返回值，导致该分支在 PCI enable 成功后本就不可达。它是修正控制流时必须一并修好的潜在缺陷，不能写成当前必经的崩溃点。
2. core 存储的是 `struct drm_lsdc_private *`，PCI remove 将其取成 `struct drm_device *`。原版 `drm_dev` 恰好是结构首成员（[lsdc_drv.h:148](/home/arch/loongson-gpu-lab/work/graphic-original-audit/lsdc_drv.h:148)），地址重合，所以本轮不把这一写法单独判成当前必然错址；重写仍应使用明确的私有对象与成员访问。

## 4. 构建与功能边界

[Makefile:4](/home/arch/loongson-gpu-lab/work/graphic-original-audit/Makefile:4) 硬编码 `/home/virgo/graphic` include 路径，`:29` 生成 `loongson.ko`。移动源码或与内核同名模块并存时容易造成构建/加载身份混淆。原版共 12 个 `.c`，Makefile 链接其中 11 个；未链接的 `loongson_device.c` 也不是一份可直接启用的完整实现。

原版 `.driver_features` 含 `DRIVER_RENDER`，但全树没有 GPU 命令提交、渲染 ioctl、调度或 shader 实现。这个标志不证明 3D/通用 2D GPU 加速已经存在。当前手册重写应继续保持“显示控制器”与“GPU 执行单元”的范围区分。

原版还使用 6.7 时代的若干 DRM/TTM API；迁移至 7.3 需要接口适配。但即使解决全部编译问题，上述 KMS 和 BO 功能缺口仍然存在，所以不能把无显示主要归因于内核版本过新。

## 5. 重写时应恢复的最小闭环

`PCI/MMIO 与共享 VRAM 所有权 → 可创建/映射/固定的 GEM/TTM BO → connector/encoder/CRTC/plane → 从手册推导的 framebuffer/时序/PLL 编程 → IRQ/vblank 与提交完成 → DRM 注册及 fbdev client → 对称失败回滚/卸载`。

保留原版分层是可行的；必须补的是每层真正承担的行为和层间接口。先完成手册范围内的单路、单模式扫描输出，再逐项验证双路、模式切换、翻页和光标。不能靠补一行模式初始化调用、放开 gsgpu 黑名单或先换桌面环境来补全缺失的显示驱动逻辑。

审查方式：完整读取原版所有 `.c`、相关接口头和 Makefile；全树限定源码检索注册/BO/中断/MMIO/清理调用；使用通用 DRM/TTM/PCI API 核对语义。原版工作树在审查前后保持 clean。本报告不包含原版编译通过、内核运行通过或硬件显示通过的声明。
