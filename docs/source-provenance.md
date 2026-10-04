# 原版来源、历史保留与新实现依据

本项目把“旧代码来自哪里”和“新代码按什么实现”分开记录。用户允许只读比较旧 Loongson KMS，以核对来源；新硬件后端仍只依据官方 LS7A2000 V1.3 手册和通用内核 API。本文不把来源核对变成旧硬件算法的移植指南，也不对作者行为作法律定性。

## 可以在本仓库复查的历史

| 版本 | 固定提交 | 用途 |
| --- | --- | --- |
| 用户原项目 | `a483dd089dfe5520fafb786568427364601f373d` | 保留重写前的文件、代码和当时的声明 |
| 首轮显示实现 | `18c116156dead4d665ff216da8c5edd33bc80e0b` | 保留这次四层架构重构前的代码、接口和限制 |
| 当前重构 | 以实际新提交为准 | 设备、memory、display、hardware 的新职责分配；验证状态见 [架构文档](architecture.md) |

在仓库根目录用只读命令查看，不 checkout 覆盖正在开发的文件：

```sh
git show --stat a483dd089dfe5520fafb786568427364601f373d
git show a483dd089dfe5520fafb786568427364601f373d:lsdc_regs.h
git show a483dd089dfe5520fafb786568427364601f373d:lsdc_region_ttm.c
git show 18c116156dead4d665ff216da8c5edd33bc80e0b:Makefile
git status --short
git diff 18c116156dead4d665ff216da8c5edd33bc80e0b -- graphic_drv.c memory display hardware
```

未跟踪的新文件要从 `git status` 识别并直接阅读，普通 `git diff` 不显示它们。

删除当前工作树中的旧文件不要求删除历史提交。不得为抹去来源记录而改写这两个基线；以后审查应能还原原样内容。

## 比较如何得出结论

2026-10-04 的审计比较了原版、`18c1161`、Linux v6.7 的 Loongson 文件和固定 `e767a4ea70a3992c37ed604157d32f0dfbf9b1e3` 的第二组 Loongson 文件。有明确线索后，只额外检查了固定 `bca45af5998a05f34b13a2ef11e639bac9c62643` 的 i915 module/VGA 两个文件。

方法包括整文件 SHA256/Git blob、去注释和空白后的连续行、保留变量名/常量/字符串的函数 token，以及人工核对。函数候选要求双方至少30个 token、匹配比例至少0.78；这个阈值只帮助定位，不能产生“原创率”或对来源的自动判决。

来源核对的固定提交、哈希、行号和方法保留在本文，可直接用上述 `git show`
命令复查。本次公开分发的第三方版权与许可全文见 [NOTICE](../NOTICE.md)。
本地实验路径不作为源码分发的依赖。

## 原版中已经核实的匹配

行号均属于固定原版与相应上游版本，不能套到重构后文件。

| 原版位置 | 上游对应位置 | 证据性质 |
| --- | --- | --- |
| `lsdc_regs.h:1–406` | Linux v6.7 `drivers/gpu/drm/loongson/lsdc_regs.h:1–406` | 整文件逐字节相同，含版权头；SHA256 `0d0c17abbb361f0dbbfbce7a34e4cbb3a7ce059953e2710a7b4fd4a801c01ee7` |
| `lsdc_drv.h:48–62` 与 `252–278` | v6.7 同目录 `lsdc_drv.h:107–121` 与 `202–228` | 两组完整私有 ops/dispatch 声明 token 同序相同，超出仅有公共 DRM 类型相同 |
| `lsdc_drv.h:113–143` | v6.7 `lsdc_drv.h:170–199` | output/pipe 结构与相邻 helper 的连续片段相同 |
| `lsdc_region_ttm.c:145–184` | v6.7 `lsdc_ttm.c:47–86` | placement 函数体225个 token 完全相同 |
| `lsdc_region_ttm.c:29–44` | v6.7 `lsdc_ttm.c:254–269` | offset 函数体73个 token 完全相同，含自定义错误文本 |
| `lsdc_region_ttm.c:109–143`、`53–71` | v6.7 `lsdc_ttm.c:288–322`、`324–342` | pin/unpin 长函数仅有局部类型/成员适配，函数体比例约0.988/0.980 |
| `lsdc_region_ttm.h:32–54` | v6.7 `lsdc_ttm.h:23–45` | 完整私有 BO 布局、成员顺序和特色注释相同 |
| `loongson_device.c:21–25` | v6.7 `loongson_device.c:20–24` | 五条 Loongson 私有回调绑定相同；该文件当时不参与构建 |
| `lsdc_module.c:41–66` | bca45af 的 `drivers/gpu/drm/i915/i915_module.c:83–109` | 初始化/回滚函数体 token 完全相同，另有特殊的四行 Early-exit 说明相同 |

只读上游出处：[v6.7 寄存器头](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/loongson/lsdc_regs.h)、[私有结构](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/loongson/lsdc_drv.h)、[TTM 实现](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/loongson/lsdc_ttm.c)、[TTM 头](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/loongson/lsdc_ttm.h)、[固定 i915 module](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/bca45af5998a05f34b13a2ef11e639bac9c62643/drivers/gpu/drm/i915/i915_module.c)。这些链接服务于来源核对，不是新实现的硬件依据。

Loongson 参考头原样记录了 `GPL-2.0+` 与 `Copyright (C) 2023 Loongson Technology Corporation Limited`；i915 module 的头为 `MIT` 与 `Copyright © 2021 Intel Corporation`。原版完整寄存器头保留了前者，一些派生片段只留下项目 `2024Ltd`/`sw` 模板。继续保留实质片段时，需要准确记录原归属与本项目改动，不能靠改名或换头把来源变掉。本次保留历史，不修改旧提交中的事实。

## 哪些相同不能直接判作复制

`drm_atomic_helper_update_plane`、`drm_atomic_helper_plane_reset` 等标准 callbacks 的表格很容易在多个驱动中相同；`container_of`、分配失败返回 NULL、释放 GEM 后 kfree 也有通用写法。应结合完整私有结构、长控制流、独特注释和上下文，而不能只比较函数名字。

在 `18c1161` 的三个新 KMS 文件中，本轮没有找到达到上述阈值的旧 Loongson 函数体匹配。发现的连续片段主要是 callbacks、include 或函数边界。memory 的短分配/清理套路和 mutex/list 小结构也单独分类，没有扩大成整份文件来源结论。这是有范围的审计结果，不证明所有可能来源均已排除，更不能替当前新架构宣称一次尚未进行的审计。

## 旧支架退出当前实现，历史仍可追溯

在 `18c1161`，以下9个源码/头文件及 `tags` 与原版 blob 完全相同，又不在该提交 Makefile/include 可达图中：

- `display/lsdc_vga.c`、`display/lsdc_vga.h`。
- `loongson_device.c`、`loongson_pci_config.h`。
- `lsdc_mitigations.c`、`lsdc_mitigations.h`。
- `lsdc_params.c`、`lsdc_params.h`。
- `lsdc_reg.h`、`tags`。

这组文件不属于新的模块边界。最终清理依照实际 Makefile/include 关系完成，不能把“文件不参与模块链接”泛化到测试和文档。也不能只按名字删除：`18c1161` 的 `lsdc_regs.h`、`lsdc_drv.h` 和 TTM 文件已是不同内容且仍参与构建。

本次新目录以 [架构表](architecture.md) 为准：需要的入口合到 `graphic_drv.c`，BO/TTM 细节收进 `memory`，KMS 对象与事务放在 `display`，手册控制寄存器操作集中在 `hardware`。改目录本身不证明重新实现；每层的接口和生命周期才是审查对象。

## 新实现的依据如何写清

| 内容 | 应记录的依据 |
| --- | --- |
| 寄存器字段、时序、PLL和中断 | [硬件契约](hardware-contract.md) 对应的 V1.3 章节/纸面页码 |
| BO/GEM/TTM 与 DRM helpers | 实际目标内核的通用文档、头文件与 core/helper 语义 |
| 设备资源、对象交接、事务恢复 | 本项目明确的所有权和并发设计；从 `18c1161` 延续的项目内容保留相应记录 |
| 排空等待、固定模式与超范围 PLL 继承 | 单独标为工程策略或已有观察，不改称手册保证 |
| 编译、运行、故障测试 | 精确提交、内核和真实执行结果；不从历史基线移植通过结论 |

代码保留适当的 SPDX/已有作者说明，文档解释来源和变化，不做整树无差别添加或删除版权头。新实现不依赖实验工作区中的旧 reference 目录，也不把旧驱动实现作为资料缺口的答案。
