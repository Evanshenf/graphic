# 从一帧像素学习 graphic

建议先看 [架构图和文件表](architecture.md)，再按下面顺序读代码。每一步都围绕“谁持有对象、谁可以失败、何时允许释放”展开。英文内核注释保留简洁的局部意图，中文解释集中在文档中；不需要逐行翻译 API 名。

本文对应已完成目标编译和实机回归的新架构；详细范围见 [验证记录](validation.md)。旧提交 `18c116156dead4d665ff216da8c5edd33bc80e0b` 是对照基线，不是新架构的验证结果。

## 1. 从设备入口找到资源的主人

先读 [graphic_device.h](../graphic_device.h)，再读 [graphic_drv.c](../graphic_drv.c) 的 PCI ID、`graphic_probe()`、`graphic_remove()` 和 `graphic_shutdown()`。

入口依次要解决 DC 窗口、桥片配置窗口、配对 GPU 显存所有权、memory、KMS、DRM 注册。DRM 设备注册放在必要子系统初始化之后，用户态才能看到一个已有对象和接口的设备。配对 GPU 的 BAR2 是共享资源；它被别的 GPU 驱动占用时不能直接接管。

对每项取得动作，找到它对应的返回值检查和释放动作。特别区分 PCI/devm 的解绑寿命与 DRM/drmm 的最后引用寿命。`remove()` 不是简单反向调用几个 init 名称；它还必须先停止异步工作和扫描，避免资源已经回收仍有访问。

读完应能沿代码指出：谁保存 CPU 和 bus 地址、谁映射窗口、哪一方最后归还 VRAM，以及为什么不再需要空的 VGA/mitigations/module 包装层。

## 2. 从一块 BO 走到 framebuffer

读 [memory/graphic_bo.h](../memory/graphic_bo.h) 的公开接口，再沿 [memory/graphic_bo.c](../memory/graphic_bo.c) 的 `graphic_dumb_create()` 找到分配、清零、用户 handle、mmap 和析构。

以 1920×1080、32 bpp 为例，一行像素是 7680 字节，正好是 256 的倍数；仍应使用驱动返回的 pitch 和 size，而不是用户态自行假定。TTM 处理存储位置和迁移，GEM handle 是当前文件描述符中的对象引用，framebuffer 再描述这块存储的格式和布局。

`graphic_bo_pin()` 的成功意味着调用者拿到了一个必须归还的 pin 和有效的 bus 地址。它不把 TTM 私有结构暴露给 display。通过这一小接口，学习时可以先理解扫描地址的稳定性，再回头读 TTM 的具体实现。

可跟读 [kms-smoke.c](../tests/kms-smoke.c) 的 CREATE_DUMB、MAP_DUMB、初始全零检查和 AddFB2。CPU hash 说明映射数据可读回；它不能单独证明显示器收到的颜色或字节排列正确。

## 3. 把 DRM 对象连成显示管线

读 [graphic_display.h](../display/graphic_display.h)，再读 output、plane 和 CRTC 的 init 函数。

| 对象 | 在本项目中的含义 |
| --- | --- |
| framebuffer | 像素存储的格式、pitch 和 offset 描述 |
| primary plane | 主画面，从 framebuffer 取一段像素 |
| cursor plane | 独立的 64×64 ARGB 光标图像及屏幕位置 |
| CRTC | 扫描时序、启停、vblank 和一次状态更新的组织者 |
| encoder | CRTC 与输出链路之间的 DRM 连接对象 |
| connector | 用户态看到的 HDMI 连接状态和可用模式 |

[graphic_output.c](../display/graphic_output.c) 只发布后端捕获的模式。HPD 表示插入状态，不提供显示器完整能力；没有 EDID 时不能据此生成任意模式。时钟另做整数相等检查，因为 `drm_mode_equal()` 对时钟周期存在取整比较。

本项目保留两路对象容器，但这不构成双显示器、所有组合和全部热插拔行为已经验证的声明。

## 4. 沿一次 atomic 请求分开看四个阶段

从 [graphic_kms.c](../display/graphic_kms.c) 的总检查和 commit tail，走到 [graphic_plane.c](../display/graphic_plane.c) 与 [graphic_crtc.c](../display/graphic_crtc.c)。

**check** 决定状态可不可以提交：模式是否等于继承快照，主平面是否覆盖当前模式，光标是否符合固定布局，偏移与最后一行是否越过 BO。坐标、长度和地址相加前后都有溢出问题；仅检查 `width <= 1920` 不够。

**prepare** 决定这次状态要持有哪些资源：先取得新 BO 的 VRAM pin，计算最终扫描区间，再记录 prepared state。这里可以失败，失败时只能归还自己取得的资源。旧 state 的 pin 此时还要保留。

**commit** 执行已准备的计划：在每路串行锁下协调硬件，写入由手册后端承担。执行时再次读取故障状态；前一个提交可能在本次 check 之后才超时，因此 check 时的状态不能作为恢复的唯一依据。

**retire/cleanup** 处理旧状态：等待事务完成，或停用并排空后取消，最后 unpin。把 cleanup 提前到“寄存器已写”会使仍被扫描的内存可迁移或复用。

读 `graphic_plane_state` 时重点看 `offset`、`end`、`bus_address` 与 `owns_pin`，再检查 state duplicate、prepare 失败和 destroy 三条路径是否维持同一个所有权规则。

## 5. 从 IRQ 回到提交完成

读 [graphic_flip.c](../display/graphic_flip.c) 的 begin、expect_bank、arm、irq、wait 和 cancel。再沿 KMS IRQ 入口走到后端 `ls7a2000_irq_read_and_ack()`。

后端隐藏硬件中断位号，只返回发生 VSYNC 的管线集合。display 把硬件现象与当前事务关联：主平面翻页要确认目标 bank，而不是把任意 VSYNC 都当作这一次翻页完成。

接着考虑没有 `DRM_MODE_PAGE_FLIP_EVENT` 的提交。用户不需要事件通知，旧 BO 仍需要安全退休。光标 BO 更换尤其容易漏掉这一步；`graphic_flip.done` 必须独立存在，不能仅根据 `state->event == NULL` 省略退休。

再读 `graphic_cursor_position_only()`：同 FB、同地址、图像/viewport/格式未变的 legacy 坐标更新立即完成。prepare 已为新状态持有同一 BO 的 pin，旧状态清理不会释放正在使用的存储。此优化不能推广到更换图像。

最后跟读 IRQ 与 timeout/cancel 如何竞争 `pending` 的所有权。event、vblank 引用和 completion 只能收尾一次。不要在 IRQ 或持有 spinlock 时做会睡眠的停止/排空操作。

当前 `graphic_flip_wait()` 先用 `10 * HZ` 等待 completion；超时后取消，若 IRQ 已领取收尾权则等待它结束，再允许旧 BO 释放。这与 smoke test 用户态每次 event 的2秒 poll 截止时间不同，不能宣称整个 ioctl 都有2秒完成保证。等待阈值与取消策略属于工程选择，应一起阅读和验证。

## 6. 只在后端阅读寄存器

读 [ls7a2000.h](../hardware/ls7a2000.h)，再对照 [硬件契约](hardware-contract.md) 读 [ls7a2000.c](../hardware/ls7a2000.c)。这个接口没有 `drm_crtc`、TTM placement 或用户 event；它只接收普通时序、地址和管线编号。

选择三个操作跟踪即可：捕获 link、切换扫描 BO、确认并清除 VSYNC。分别核对“不调未知 PHY 频率”“在正确 bank 写入新地址”“W1C 状态不能做普通整字读改写”。随后再看 cursor 和 disable 的帧时间排空。

手册写明、旧基线观察、当前工程选择是三个不同来源。继承一个已锁定但 VCO 低于手册下限的 PLL，并不使这个参数组合成为手册推荐配置；帧时间等待也不是芯片报告 DMA idle。

## 7. 从异常路径检查架构是否闭合

按以下场景跟读，不急于在实机注入错误：

| 场景 | 应能找到的处理关系 |
| --- | --- |
| BO 分配、映射或 prepare 失败 | 只释放本次拥有的引用/pin，旧可用状态不被提前退还 |
| primary 没变，只换 cursor BO，且不请求用户事件 | 新旧 BO 各自有明确 pin，旧 pin 在完成边界之后释放 |
| 前一提交超时，本次早已完成 atomic_check | commit 执行时重新看 fault，并能重放完整 primary/cursor 计划 |
| IRQ 与 cancel 接近同时发生 | 只有一个路径领取收尾权，没有双发事件、双 put 或遗失 completion |
| mode/PLL 继承条件不再成立 | 故障可见，停止/取消先于 BO 退休，不擅自生成替代频率 |
| 初始化半途失败或卸载 | polling/IRQ/扫描先停，KMS 与 memory 按对象寿命回收，最后归还 VRAM |

硬件验证前确认正在测试的提交、内核和模块确实对应，已有图形客户端/SSH/恢复方式的状态也应在执行时检查。测试程序需要独占 KMS；运行成功只覆盖它实际执行的用例。无事件光标更新、故障注入和强制解绑需要单独的验证记录，不能从十次有事件翻页结果推断。

可先在目标内核环境构建：

```sh
make W=1
cc -std=c11 -O2 -Wall -Wextra -Werror tests/kms-smoke.c \
  -o /tmp/graphic-kms-smoke $(pkg-config --cflags --libs libdrm)
```

上面是复现入口，不是已执行或已通过的声明。驱动加载、桌面切换与恢复应遵循当次实机计划，不能仅凭一份文档照搬旧会话的现场条件。

## 通用 API 的进一步阅读

以目标内核对应提交中的文件为准：`Documentation/gpu/drm-kms.rst`、`drm-kms-helpers.rst`、`drm-mm.rst`，以及 `include/drm/drm_atomic*.h`、`drm_modeset_helper_vtables.h`、`drm_managed.h`、`include/drm/ttm/`。7.3 的实际类型和 helper 所有权以源码为准，旧教程的回调签名不能直接套用。

需要回看原项目时，使用 [来源记录中的固定 Git 命令](source-provenance.md)，把历史比较与当前实现依据分开。
