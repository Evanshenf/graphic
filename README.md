# graphic：可阅读的 7A2000 显示驱动

权威开发目录是板上的 `/home/virgo/graphic`。当前分支 `graphic-architecture`。
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

目标内核为 `7.3.0-rc3-gpulab-bca45af`，输出仍为 `graphic.ko`。
DRM core/KMS helper 内建；TTM 和 TTM helper 保持独立模块。

```sh
cd /home/virgo/graphic
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
