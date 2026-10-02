# 源码归档与主干合入 — 2026-10-02

用户要求将 shadPS4 和 my_mcp_tools 当前工作统一检查、合理提交、合入主干并推送，包含工具主工作区此前遗留的源码与文档。本批不进行设备测试。

## shadPS4

fork 主干是 `origin/malos/main`，工作分支为 `feature/malos/swan_performance`。本批基线 `85a39824d`，新增：

- `92684ca35`：[GPU query 回收修复](gpu-query-recovery-20261002.md)，完成提交中缺失结果不再永久堵住租约；不完整批次明确标记，不伪造耗时。
- `5fac32aef`：[按游戏隔离软件插值实验与 GCN 清查](gcn-software-audit-20261002.md)，包括 [PC / Android 实现文档](../../guides/gcn-emulation.md)、能力 probe 和小型证据。
- `7be64f379`：[血源同窗 CPU / GPU 证据](bloodborne-live-bottleneck-20261001.md)，保留旧采样条件及归因边界。
- 本记录和 AGENTS 状态更新；原始证据的展示副本去除行末空白，原文件仍在报告指向的忽略目录中，新 manifest 记录展示副本 SHA。

`main` 是官方 `upstream/main` 的独立参考分支。本批拉取官方最新引用 `3912336fd`，没有把官方新功能额外混入 fork，也不改写已共享历史。Foundation `530a60a`、Mesa `9b8a35676e` 等子模块引用不变，工作树均干净；两者已有对应远端 `codex/shadps4-xr-*` 分支包含，无未发布的新依赖。

验证沿用本批源码已经完成的 Android host / probe / APK 构建与设备正负对照：GPU timing 27/0、生产 Vulkan probe 159/0、旧实现负对照 159/47；GCN 实验隔离 Mac / Android 各 11/0。提交前又运行 Mac UBSan 11/0，核对文档的 68 个本地链接。当前 GCN 候选 APK 未安装；没有把源码提交或构建成功等同于新的游戏性能验收。

## my_mcp_tools

主干为 `origin/master`。基线更新到 `6568542`，包含已有 scrcpy SnapshotSession、XR 图源以及 Windows 修复。合入 GPU coverage / RenderDoc 工作量归因、LiteP SDK 身份选择与采集失败清理、Android 自动化实现，以及 09-23 的设计和验证文档；旧 README 中错误的路径变更已解决。

工具的完整记录在该仓 `Docs/Validation/2026-10-02-Source-Integration.md`。离线测试：LiteP 120/0，日志句柄修正后定向 5/0；RenderDoc application 82/0、MCP contract 56/0；Android 自动化 129/0，包含真实 Apple Vision 与 H.264 编解码。未发布新 MCP 二进制、未改已安装版本指针。

## 保留的边界

- 血源重新采集因共享 AYN 被另一个 session 使用，按用户要求暂停；没有新的无干扰归因数据。
- 原始 PROF / RDC / shader cache、APK、编译目录与虚拟环境保留为忽略的本地产物，不混入源码。
- 历史报告中的进程、配置及“无 commit/push”描述是测量当时状态，不代表归档后仍有源码未提交。
- Swan GPU hang、游戏性能收益和当前 APK 上的自动化 profile 重新标定均未在本批验收。

## 晚间归档（同日）

用户要求提交并推送当前工作区，合入主干，并且其他会话遗留的改动也要正确提交。基线 `1c56a3f1f`，按功能拆成 24 个代码提交加本文所在的文档提交，推送 `feature/malos/swan_performance` 后快进合入 `malos/main`。

**子仓（先推子仓，再更新父仓指针）：**
- Foundation `2e81e83`（`codex/shadps4-xr-foundation`）：`perf_metrics` 的 Windows CPU/GPU/内存/电池数据源。
- Mesa `64817e11155`（`codex/shadps4-xr-turnip`，`tencentmalos/mesa-mirror`）：Turnip 暴露 `VK_KHR_fragment_shader_barycentric`。这是 MHR 会话留在子仓工作区的补丁，按该仓 AGENTS.md 只写最短提交信息并带 `Generated-by: LLM`。

**10-01 MHR 会话遗留（[记录](mhr-20261001.md) 12:55 起两节）：**
- `8d3082e31` Turnip 源码构建关闭 android-strict，并更新 Mesa 指针。
- `e34eff03c` AvPlayer `JumpToTime` 真实 seek。
- `24fb05d38` 动态图像表按 command processor 恢复周期只读一次。
- `6fc855e63` 已知 mip 的采样偏移改为坐标增量（shader binary 35）。
- `3544d713a` GCN min/max/med3/clamp 的 NaN 规则与 probe（binary 36）。
- `6f2fd7d1a` DMA 同步边界默认开启、texel 读取按写版本跳过、深度/颜色拷贝开关。
- `92c9a1a85` 256 线程常量填充与原始拷贝核。
- `a71435c0c` 深度/颜色 twin 复用。
- `5e7ca3130` `draw_skip`、`gpu_images` 按层导出、缩放钉住日志。
- `36de91f30` RenderDoc 时给 KGSL SVM 留出地址窗口。
- `c0ba25b36` 插值 probe 覆盖驱动 barycentric。
- `636296eec` MHR 记录。

**本会话：**
- `26c844889` Foundation 指针与桌面状态指标文档。
- `dd145b6e5` guest patch sdk_version 2（site、同长度补丁、桌面加载器）；`fb1d6f075` 血源 1.00 60 FPS 包；`a713f7aa0` Big Picture 启动选项；`e939b294b` 手柄首个输入成为 Player 1；`f7708b86a` DebugBus `desk_pad`。
- 桌面瓶颈修复：`eb4e52646` 深度模板重采样，`1701e0da5` 上传偏移 16 字节对齐，`55e17357b` stream DMA，`7ea88936c` arena 上传计数，`a82a22d2a` 纹理绑定缓存，`0bb0223ad` epoll 空转。

**拆分方式：** 同一文件里两次会话的改动，用只写 index 的分块暂存拆开。三个交错文件先备份整个工作区，再临时写入只含 MHR 改动的版本，提交后从备份恢复。提交后核对：代码目录与提交前测试用的工作区逐字节一致。中间提交没有逐个编译，以最终提交为准。拆分时有两处零上下文插入块被 `git apply` 放错位置（`CMakeLists.txt` 的 probe 目标、`vk_rasterizer.cpp` 的 `NoteWrite`），已在推送前改正。

**验证：**
- 桌面：沿用瓶颈报告第 9 节的构建与 A/B。
- Android：host `HOST_LINK_PASS`，源码为 `8d3082e31` 加当时的工作区，内容与最终提交相同。APK `3e8e160a…` 打包时跳过了 `compressXrCinemaTextures`（本机没有 `astcenc`，XR 影院贴图未打入）。
- 设备：Pocket DS 实测血源，见 [瓶颈报告第 10 节](bloodborne-desktop-bottleneck-20261002.md)。

**未提交：**
- `externals/mesa-kosmickrisp`：检出在另一条上游历史的 `c502a84`，不是本批改动。
- 未跟踪的 `externals/dear_imgui/`、`externals/imgui/`。
- `build/drivers` 下的本机 Turnip lock，以及 PROF 等本地证据。
