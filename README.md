# graphic：可阅读的 7A2000 显示驱动

graphic 是面向学习的龙芯 7A2000 Linux DRM/KMS 显示驱动，重点是可读的职责分层、
对象寿命和真实硬件验证。目前实现单路固定模式显示，不包含 GPU 命令执行、
2D 绘图加速或 3D 渲染。硬件后端依据官方 LS7A2000 V1.3 手册。

公开仓库默认分支为 `main`；板上开发仍在 `/home/virgo/graphic` 的
`graphic-architecture` 分支。开发过程使用了 OpenAI Codex 辅助，提交说明中有记录。
原版保留为 `a483dd0` / `graphic-original`；首个实测版本保留为
`18c1161` / `graphic-display-v1`。旧代码从活动源码删除，历史仍可审查。

## 从哪里读

1. [学习路线](docs/learning-guide.md)：从一幅图如何显示开始，按调用链读代码。
2. [架构](docs/architecture.md)：职责、对象、锁、引用和失败回退。
3. [硬件契约](docs/hardware-contract.md)：每个硬件假设的手册依据及缺口。
4. [来源审计](docs/source-provenance.md)：原版移植/摘录部分的固定出处。

## 代码地图

| 文件 | 负责的事情 |
| --- | --- |
| `graphic_drv.c` | PCI 注册、资源接管、各层初始化和停止 |
| `graphic_device.h` | 顶层设备及子系统所有者，子系统内部保持私有 |
| `memory/graphic_bo.c` | GEM/TTM 分配、清零、映射、固定和释放 |
| `display/graphic_kms.c` | 原子检查、完整提交计划、初始化和 IRQ 分派 |
| `display/graphic_plane.c` | 像素缓冲区检查、每个状态的 pin 和扫描地址 |
| `display/graphic_crtc.c` | 扫描管线启停与执行阶段恢复 |
| `display/graphic_output.c` | HDMI 连接状态和模式公布 |
| `display/graphic_flip.c` | 有事件/无事件提交的完成、等待和取消 |
| `hardware/ls7a2000.c` | 唯一的寄存器读写与硬件序列，不依赖 DRM/TTM |

没有空的 VGA、mitigations、params、i915 初始化框架，也没有全局混装
PCI、TTM、plane/CRTC、寄存器和事件的头文件。通用 DRM API 相同不能单独
证明复制。原版来源和保留代码的归属记录见来源审计。

## 构建

实测内核为 `7.3.0-rc3-gpulab-bca45af`，对应内核提交
[`bca45af5998a`](https://gitlab.freedesktop.org/drm/misc/kernel/-/commit/bca45af5998a05f34b13a2ef11e639bac9c62643)，
输出为 `graphic.ko`。尚未验证其他内核版本；内核内部 DRM/TTM API 可能需要适配。
构建需要配置并准备好的目标 LoongArch 内核树、匹配的工具链；测试程序还需要
libdrm 开发文件，鼠标测量工具需要 X11 开发文件。默认 KDIR 是运行内核的 build 目录，
也可执行 `make W=1 KDIR=/path/to/target-kernel-build -j8` 指定匹配的构建树。
DRM core/KMS helper 内建；TTM 和 TTM helper 保持独立模块。

```sh
git clone https://github.com/Evanshenf/graphic.git
cd graphic
python3 tests/check-architecture.py
make W=1 -j8
cc -std=c11 -O2 -Wall -Wextra -Werror tests/kms-smoke.c \
  -o /tmp/graphic-kms-smoke $(pkg-config --cflags --libs libdrm)
```

KMS smoke 必须独占显示设备，测试前保存桌面工作并停止显示管理器。
测试包括初始零、映射、1080p、vblank、翻页、光标和资源释放。
本次验证见 [验证记录](docs/validation.md)，包含鼠标输入对照和故障恢复。

## 当前硬件范围

软件架构覆盖完整的资源、提交、中断、失败恢复和退出流程；显示功能先
继承固件已经配置的单路 1080p 模式和 PLL。当前 ARGB 光标为紧凑 64×64。
DDC/EDID、PHY 调频、32×32光标布局、GPU 执行与 suspend/resume 尚未实现。
某些固件继承 PLL 参数低于手册 VCO 下限，会明确报告；不自行猜测新参数。
启动接管已实测；双显示器及打开 fd 的强制解绑需另外验证。

## 参与与许可

欢迎通过 [Issues](https://github.com/Evanshenf/graphic/issues) 反馈可复现的问题，
或通过 Pull Request 讨论接口、教学文档和硬件支持，见 [贡献说明](CONTRIBUTING.md)。
实机原始记录已随源码提供在 [docs/evidence](docs/evidence/README.md)。

当前驱动遵循 [GPL-2.0](LICENSE)，独立鼠标测量工具的 MIT 声明遵循
[MIT 许可](LICENSES/MIT.txt)。逐文件 SPDX 声明保留各自范围。历史中的 Loongson
与 Intel 片段及原作者声明见 [NOTICE](NOTICE.md) 和来源审计，历史不改写。
这是独立实验项目；后续主线投稿需要针对上游驱动准备独立补丁并经过人工审阅。
