# 2026-10-04 实机验证原始记录

对应代码提交 `3fe3637f96a8062b2afce553d1210acc11f1bf25`，最终模块 SHA256
`7c7f9d2357ce36d580521bba42bd5f0f579d680c5fbf863ce2a3d426bec006d6`。
实测目标内核为 `7.3.0-rc3-gpulab-bca45af`。本目录保存实际记录，未把
中间模块测试冒充成最终版本测试，也不包含固件/手册或已编译模块。

| 文件 | 对应阶段 |
| --- | --- |
| [mouse-baseline-lane.csv](mouse-baseline-lane.csv) | 优化前模块 `2bd07997…`，同一 LightDM 区域 |
| [mouse-fixed-lane.csv](mouse-fixed-lane.csv) | 中间坐标优化模块 `b4108e5f…`，同轨迹 |
| [mouse-final-boot.csv](mouse-final-boot.csv) | 最终 `7c7f9d23…` 模块重启后的输入/硬件位置采样 |
| [cursor-phase-test.txt](cursor-phase-test.txt) | 最终模块故障恢复、换图像、单移动、300次移动与 smoke |
| [checkpatch-final.txt](checkpatch-final.txt) | 14个生产 C/H 的严格风格检查 |

CSV 的 INPUT 行提供输入与匹配坐标时间，HW 行提供只读寄存器采样。
未匹配点不计入延迟分位；测试不测屏幕像素或光子。硬件/工具路径和 MMIO 地址
属于当时测试记录，不应当作为其他板卡的默认地址。

故障日志中的超时是主动屏蔽 VSYNC 的预期记录；记录里的 `ready for submission`
是 checkpatch 固定输出，不表示已满足上游审阅或投稿要求。最终总体范围见
[validation.md](../validation.md)。
