# RenderDoc、litep、ImGui 与 Guest trace 接入审核

2026-09-14，主仓 `b382addd`。这是源码审核与下一阶段规划依据，不是工具已接入或抓帧成功的证据。用户最新决定：**auto tag 本阶段延期，不作为依赖或验收门槛，不修改 FEX。**执行要求见 [整体验证 spec](../../specs/android-graphics-debugging-toolkit.md)。

## 已确认的基础与缺口

| 项目 | 当前主仓 | Citron / Foundation 可参考部分 | 必须解决的接入问题 |
| --- | --- | --- | --- |
| RenderDoc | `src/video_core/renderdoc.cpp` 有 API wrapper；实际加载入口在 desktop `src/emulator.cpp` | Citron `src/core/tools/renderdoc.*`、`process_diagnostics.cpp`、Android capture/replay helper | Linux/Android NOLOAD 已成功时没有 GetAPI；缺空符号检查；进程全局非原子 capture_state；无 Session API/status/file receipt |
| 抓帧边界 | Liverpool Process 调用 Start；submit_done 时先 End，再 OnSubmit/Flush | Citron explicit device/window、GetCapture、burst/frame control | 现有区间不保证包括真实 Vulkan flush/Presenter；GPU drain 批次不是 PS4 guest frame；需多队列及 flip/present 关联 |
| litep | Foundation debugbus 已链接，但 `FOUNDATION_DEBUGBUS_BUILD_PROFILER=OFF`；Android 强制 Tracy OFF，注明旧 pin 不适合 dlopen | Citron `common/profiler.*`；新 Foundation ProfilerRing + profiler_sdk，PROF ring/file/socket | 当前 Foundation `5388ef45` 没有新 SDK 子仓；不能把旧 Tracy 开关打开就声称 litep 可用；single process owner、侧车版本与 decoder 一致性 |
| ImGui | 正在实际 Presenter 路径 NewFrame/Render；游戏图像本身用 `ImGui::Image` 合成；旧 Layer raw pointer 全局队列 | Foundation `modules/imgui` Layer/LayerManager；Citron NativeLayer + Vulkan StatusLayerRenderer | 当前 ImGui 1.93.0 WIP，Foundation 1.92.2b；脏目录 dear_imgui 1.92.0 WIP 不是目标。版本/ABI/动态字体协议、游戏合成保留、Android input owner、上下文销毁 |
| guest command trace | GNM guest adapter 已校验并复制 DCB/CCB，但把 host copy 指针传给 desktop | Citron `guest_command_trace*` 的 action/resource provenance | 必须在复制前保留真实 guest VA/范围/owner/invocation，并跨队列传递；不能把 host vector 地址当 guest 地址 |
| GPU 指令 trace | Liverpool 有 DCB/CCB/ACB、nested IB、draw/dispatch/同步解析 | Citron `maxwell_trace*` bounded recorder/reader 模式 | PS4 PM4/GCN 格式必须本仓实现；不能复制 Maxwell opcode、ASID 语义、magic/layout |
| guest auto tag（仅研究，已延期） | 当前 API 未见 Dynarmic 式 instruction pre-translation hook | Citron GuestAutoTag、Dynarmic64 PreCodeTranslationHook、identity/site tests | FEX GenerateIR 的 CustomIR 分支跳过正常译码；不是保留原指令的 probe。不能修改 guest 字节或把异步 CPUState 当精确 RIP |
| 公共证据 | 本仓 Foundation 已有 gpu-snapshot 容器工具 | `foundation.gpu-snapshot.v1`、`foundation.gpu-action-index.v1` | 添加 `ps4.guest-command` / `ps4.pm4.command` 子格式；容器 hash 校验不等于同次运行/同帧/像素正确 |

## 参考身份与可取得性

[文件 hash 清单](../../data/graphics-debug-tooling-reference-2026-09-14.json) 记录本次实际读到的版本。

- 主仓：`tencentmalos/Bachata-S4`，`codex/android-fex-round2`，已推送 `b382addd`。
- 本仓 Foundation：`5388ef45313d6c32cb5f4bb5b07f1246ee381370`，owned branch `codex/shadps4-android-fex-v0`，远端一致。
- 参考 Foundation：`2b2683ff765e8aee90e7f8eca66d2476591094f9`，远端 `codex/profiler-ring-live-use` 已确认存在；[profiler guide](https://github.com/tencentmalos/foundation/blob/2b2683ff765e8aee90e7f8eca66d2476591094f9/docs/guides/profiler-ringbuffer.md)、[GPU evidence guide](https://github.com/tencentmalos/foundation/blob/2b2683ff765e8aee90e7f8eca66d2476591094f9/docs/guides/emulator-gpu-evidence.md)。其 profiler_sdk 是已有独立子仓 `107a620a64b0ac91f49e5de37ffc6d486ec42378`，远端为原 `code.byted.org`；另机需要该来源访问能力，不应把源码转发布到 GitHub。
- 本机 Citron：`/Users/bytedance/workspace/emulations/switch/citron`，实际 HEAD `6a86baf4b6521ca3ee5b2f5e2a0ca6a8e6007cc0`。本次读到的关键文件无未提交变动；**远端同名 `feature/malos/botw_performance` 仍为 `2106bcd83844e05dfbb5251a902143233a7ec43b`，GitHub commit 查询对 6a86 返回 422**。不要给另机写一个不可检出的“已发布参考 pin”，也不要自动 push 整个 Citron 工作分支。当前机器可按 hash 只读迁移；异机需取得相同参考源码或以本 spec 的独立契约实施，并记录差异。

## 不应延续的推断

1. 上次 120 秒结束是测试主动 Stop；op56 已修复。161 presents 和显示层黑屏是排查入口，不能直接定性 GPU hang，也不能据此跳过 game-only screenshot / 阶段计数。
2. Android 截图不等于 guest 最终渲染图；必须增加带 present identity 的游戏图像读回，再与合成后图和实际显示观察对照。
3. 现有 RenderDoc MCP 的 `capture_launch` 在此前同日验证中为 ABI unsupported，见 [旧记录](avplayer-directory-integration-2026-09-14.md)。实施时重新探测能力，不能靠工具名字宣称能启动；app-owned capture + 匹配 helper/server 是独立可实现入口。Android RDC 在匹配 Android/Turnip 上远程 replay，不在 macOS 本地 Adreno replay。
4. litep 的能力是读取/分析，PROF 生产者是 Foundation/profiler_sdk；Tracy 是已有兼容后端，不是新版 PROF ring 的唯一实现。不要照抄 Citron AGENTS 中早期“Foundation Tracy 就是 profiler”的历史概括。
5. Foundation guide 明确记录 SDK Android Streaming 性能门槛有失败；采集可读不代表零开销。必须实测 disabled/ring/streaming 与 GPU timestamps 的不同成本。
6. 本次仅新增规划文档，没有修改 Foundation/FEX/Citron、安装 RenderDoc Server、启用采集或实现新 trace。
