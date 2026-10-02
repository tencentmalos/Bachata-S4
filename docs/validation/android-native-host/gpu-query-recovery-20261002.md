# GPU query 回收修复与血源复采边界 — 2026-10-02

## 故障与改动

旧 collector 在 `getQueryPoolResults` 返回 `VK_NOT_READY` 或 availability 缺失时直接退出本次回收。即使对应提交已经完成，队首缺失 query 仍会持续占用租约，阻塞后续批次，最终使 GPU coverage 没有新数据。

现有 [GpuProfiler](../../../src/video_core/renderer_vulkan/vk_gpu_profiler.cpp) 先核验 submission receipt 和完成 timeline，之后才读取 query。对完成后仍缺结果的批次做 30 次有界轮询，再释放租约、标记 frame sum 不完整，并记录 unavailable/unwritten query、阶段和端点。关闭录制后仍会回收。没有使用 Vulkan WAIT，也不生成零耗时补值。

新增 `incomplete_batches`、`unavailable_queries` 和 `unwritten_queries` 状态以及 `GPU.IncompleteBatches` 计数，供采集端识别数据损失。此修复解决 collector 被缺失结果永久阻塞；**真实游戏中最初为何缺失 timestamp，仍未证明是 Turnip 哪个具体问题。**

## 验证

- 通用 GPU timing 测试：27 checks / 0 failures。
- Android 生产 Vulkan probe：159 checks / 0 failures。故意不写入一个 begin query，确认 receipt 未发布、timeline 未完成时均不回收；完成后第 30 次轮询丢弃，并继续回收后续 40 个批次。
- 旧 collector 负对照：同一 probe 159 checks / 47 failures。
- Android host、相关 probe 与 APK 已编译。装机包 SHA `9483b4f48e4a749a3a90f6b649730f40200a9968ec1a1e42ee32eb89add6975f`，host SHA `9b93041c0496229349fdfe62168ee16c062dea5da67033a747a2ecbc32ccc7db`，实际 Turnip SHA `8294375420b186838c17c710a191be8849701ddf51d2d530e534864a812f8794`。
- 实际十秒 PROF/KGSL 采集已完成；后续细采样因另一项 guest fault 失败。全局软件插值实验的处理见 [GCN 清查](gcn-software-audit-20261002.md)，不能用失败采集声称完整 GPU coverage 或优化收益。

小型可提交证据见 [evidence](evidence/gpu-query-recovery-20261002/manifest.json)。完整本地产物位于 `build/validation/gpu-query-recovery-20261002/`，不提交 APK、PROF、shader 或游戏数据。

## 复采暂停状态

用户随后要求分析进入当前血源存档后的场景，怀疑 CPU 受限。17:29 的 PID 19072 在选“继续”后被 Android 以 `LOW_MEMORY` 终止，记录 RSS 约 6.9 GB，当时尚未开始正式 PROF/KGSL 采集。重启 PID 23057 后，另一 session 开始使用同一 AYN 并启动 xrgame_native；用户明确要求等待，设备操作和采集已暂停。

菜单阶段 query 曾持续回收（2,602 retired / 4 pending / 0 incomplete），这不等于游戏场景已验收。复采没有新建 tracefs instance，没有遗留文件 capture；本地分析 controller 已关闭。后续应重新核验设备独占、PID/配置与场景，再取得同窗 CPU/GPU 数据。最新暂停记录在本地 `build/validation/bloodborne-hotspots-20261002/PROGRESS.md`。

## 工具侧配套

my_mcp_tools 的 LiteP 修复 SDK 身份选择、包内 helper 定位、文本诊断保存、job ID 返回以及 PROF 启动失败后的有界 KGSL 清理，完整 Python 测试 120/0。使用源码 controller 的最后一轮未形成新的无干扰采集；已安装 `gpuquery4` 也不等于包含最后的清理修正。源码合入与 MCP 二进制发布分别记录。
