# 本地修改检查与发布（2026-10-23）

本轮基于 `bb8175cb3501aac0c3d9a4b62ba31f11f673cc69`，检查并归档已有工作区修改，
交付到 `feature/malos/swan_performance`。历史记录中的“本地未提交/未推送”描述对应采集时点；
本轮没有操作设备、安装 APK 或复测游戏，不新增性能和兼容性结论。

## 归档范围

- Guest 调试器：模块身份、watch 命中归属、缓存单指令插桩、REP/AVX/x87 访问与 raw PF/AF
  寄存器保全，配套 fixture 和测试。见 [调试器记录](guest-debugger-watch-20261015.md)。
- 呈现：Android 预旋转、游戏画面直接进入 swapchain、ImGui 叠加层与空闲重画、采集回退、
  device-lost 错误传播和录制线程收尾。见 [呈现记录](present-prerotation-20261015.md)。
- KH3：AvPlayer 回调争用修复、DrawIndex 叶子路径默认关闭、REWIND guest 原始地址修复，
  以及默认关闭的命令/缓冲/图像生命周期诊断。见 [闪黑诊断](kh3-flicker-controls-20261014.md)。
- A8xx：Turnip 子仓、KGSL 最小复现、Vulkan 能力探针、A/B 与 trace 分析脚本、故障报告及交接。
  见 [DDE 记录](a8xx-dde-opcode-zero-20261017.md) 和 [最小复现](../../../tools/a8xx-dde/minicase/README.md)。
- 文档：Android Windows 游戏运行路线草案、PROF 解析缺陷、已有性能摘要及 Adreno SP/wave
  讲解素材。视频作为 `docs/media` 的正式文档附件，与前两集采用同一归档方式。

## 本轮检查发现并修复

`CommandRecorder::Abandon()` 原先先设 `broken`，再调用 `Sync()`。如果工作线程已有待报告异常，
`Sync()` 会在 `Handoff()` 中抛出，跳过后面的 idle 等待。此时工作线程可能仍在销毁当前命令的
捕获对象，presenter 却已开始销毁它们引用的渲染资源。

改为直接丢弃生产者尚未提交的命令，再等待工作线程完成当前 chunk 并丢弃已排队 chunk，最后清除
异常。新增回归用例以阻塞捕获对象析构的方式固定该竞态，验证等待发生、未提交命令不执行、后续
`Sync()` 不重放被丢弃的命令。

## 本轮验证

| 检查 | 结果与边界 |
|---|---|
| 录制线程回归 | Windows MSVC 编译实际 `vk_command_recorder.cpp` 与仓内测试，14 checks / 0 failures；仅线程命名/优先级、profiler、设置和断言接入用本地 stub，Vulkan 调用用测试回调，无 GPU |
| 负对照 | 同一测试换回检查前的 `Abandon()`，14 checks / 1 failure，失败项为等待 worker cleanup |
| Android host | NDK 29 / API 33，`shadps4_host` 编译链接通过；存在已有 Foundation 和 Vulkan 初始化警告 |
| Android 测试目标 | `guest_execution_tests`、`guest_avplayer_tests`、`command_recorder_tests` 编译链接通过；本轮未在 Android 上运行，历史 259/0、93/0 等结果不冒充本轮执行 |
| 工具 | KGSL 静态最小复现与 Vulkan 能力探针交叉编译通过；六个 Python 文件语法检查、四个 shell 文件 `bash -n` 通过 |
| Git | 修改及暂存差异空白检查；显式选择路径；子仓先推送并核对远端引用 |

首次 Android 构建因 Windows 缺少 POSIX 环境、Turnip 源码构建不支持 Windows 而失败。
按现有构建脚本设置 Git `usr/bin` 到 PATH、`SHELL`，并使用 `SHADPS4_TURNIP_PREBUILT` 指向
`build/turnip-dde-keep` 后通过。该入口验证 Mesa 源码树与已有驱动完全一致；驱动 SHA-256 为
`d7e6746a5eb02500f8d4ac3022ae8459a3e683b56ba55b6dc2705b5706f46099`。
没有改用其它版本驱动，也没有改构建配置文件。当前 host/FEX/探针和本地测试辅助文件位于
`build/`，不入仓。完整桌面应用、APK 打包和设备运行未重新验证。

## 子仓及保留项

- Turnip：`68dafb89a26adefeaa12411f08e841aaf0b57384`，推送到 owned fork 的
  `codex/a8xx-dde-merge`；12 个待发布提交均已有 `Generated-by: LLM` 尾注。
- Bachata-S4：`bd230a27c427fed4f39f882f5b933820ceb1df69`，推送到
  `fix/windows-invalid-zone-identifier-paths`，只清理 Windows 不合法的下载元数据路径。
- Foundation：既有 pin `80d25c5f07b30db40efc6d51a36d4a4a736cf08d` 已存在于远端
  `codex/shadps4-oboe-stall`，本轮未改 pin 或源码。
- `externals/mesa-kosmickrisp` 是比父仓 pin 更旧的本地检出，不纳入版本回退；
  `externals/imgui` 和 `externals/spdlog` 为独立本地 checkout，保留在原处。
- 未跟踪的 `cs_0x000000000934eeac_0.bin` 是游戏 shader 原始证据，按仓库不提交游戏二进制的
  规则保留本地。没有删除、清理或强制重置上述保留项。
