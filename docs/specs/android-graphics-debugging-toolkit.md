# Android 图形与性能调试工具整包

状态：**待实施 spec**。2026-09-14；交给下一位实现者（Opus 4.8）。代码基础为主仓 `codex/android-fex-round2` / `b382addd`；[源码审核与参考版本](../validation/android-native-host/graphics-debug-tooling-audit-2026-09-14.md)、[来源 hash](../data/graphics-debug-tooling-reference-2026-09-14.json)。

**用户最终范围：RenderDoc、litep、ImGui Layer、guest command trace、GPU 指令 trace 一并完成；guest auto tag 暂不做。** 不实施自动地址探针、guest 二进制补丁、FEX 翻译钩子、逐条 x86 执行 trace 或语义函数自动标注。无需为本 spec 修改 FEX。Guest 来源先利用已存在的 HLE/提交边界，只输出能够证明的身份。

## 1. 最终目标与交付方式

交付一个普通 APK 中可用的调试工具包，让用户能在 TMNT 黑屏、卡帧或文字异常时：

1. 从 ImGui 调试面板或 adb/debugbus 查询 guest、命令处理器、host submit、GPU retire、present 分别是否推进。
2. 获取带明确帧身份的 game-only / with-overlays 图像及 RenderDoc RDC，在同一 Android/Turnip 上 replay，检查 draw/dispatch、pipeline、shader 和绑定资源。
3. 用 litep 获取最近历史或连续时间段，区分 FEX 执行、HLE、PM4 decode、shader 编译、GPU 同步、合成、present 的耗时。
4. 从 RDC action 的 marker 追到 PS4 guest action、PM4 packet、DCB/CCB/ACB 或 nested IB、提交调用与资源地址范围。
5. 将以上证据及源版本、配置、完整性标识打成一个可验证的包，交给另一台机器继续分析。

按两个工作包连续实施，内部可以分提交，但**不再按单个功能写微型 spec、停下来请求下一步**。完成 host-only synthetic 后进入普通 APK synthetic，再对当前真实 TMNT 做一次问题场景采集。目标是调试工具可用、证据链可追溯，不要求本轮修完 TMNT 全部渲染问题或实现在线网络/SSL。不要把一次存活或 RDC 文件存在写成游戏可玩。

保持 ARM64/bionic host + FEX x86-64 guest、Turnip 优先、4KiB、host/JNI RelWithDebInfo。保留 debug APK、匹配的未裁剪符号和 Build IDs；不切成全 Debug 来掩盖时序问题。Swan/API36 为目标；现有 AYN/API33 可以执行辅助验证，不能冒充 Swan。

## 2. 复用边界与准备工作

### 2.1 可以直接作为实现参考的路径

Citron 本机路径与可取得性见审核；**本机 HEAD 6a86baf4 尚未发布，远端同名分支仍是 2106bcd8**。不能假定另机 clone 就有新版代码。本 spec 的接口/验收独立成立；若取不到新版参考，要明确记录，不得伪造参考可达性或擅自 push Citron 全部分支。

| 能力 | Citron 参考 | shadPS4 实际接入点 |
| --- | --- | --- |
| 控制与生命周期 | `src/core/diagnostics/process_diagnostics.cpp`、Android `jni/native.cpp` 的 DumpsysBridge | 新 `src/core/diagnostics/`、SessionCore/production runtime、app 自有 DebugDumpService/JNI |
| RenderDoc | `src/core/tools/renderdoc.*`、`src/video_core/renderdoc_scope.h`、`skills/citron-renderdoc-analysis` | `src/video_core/renderdoc.*`、`vk_platform.cpp`、`vk_presenter.cpp`、Liverpool |
| litep 生产者 | `src/common/profiler.*`、Foundation `ProfilerRing`/`LiteTrace`/`ProfilerCommands` | 新 profiler 门面，host/guest 边界、Liverpool、scheduler、Presenter、shader/cache |
| Layer/Android 输入 | `src/video_core/status_layer/*`、`renderer_vulkan/vk_status_layer.*`、Foundation `modules/imgui` | `src/imgui`、现有 Vulkan ImGui backend、Foundation input/NativePadBridge |
| decoded guest action | `src/video_core/guest_command_trace*`、`tools/gcmdtrace` | 新 PS4 guest action recorder/decoder，GNM submit / Liverpool / Rasterizer / cache |
| 原始 GPU 命令 | `src/video_core/maxwell_trace*`、`tools/maxwell-trace` | 新 PM4 recorder/decoder；不复用 Maxwell wire layout/opcode |
| 证据包 | Foundation `tools/gpu-snapshot`、`docs/guides/emulator-gpu-evidence.md` | 新 PS4 子格式和规范化 action index，公共容器/关联器复用 |

### 2.2 Foundation 与第三方

- 主仓 Foundation 继续使用 owned `codex/shadps4-android-fex-v0`，当前 `5388ef45`；参考 `codex/profiler-ring-live-use` / `2b2683ff` 已在相同 owned remote 可达。**做选择性合并/迁移并审查 CMake 与输入兼容，不盲目替换 gitlink。** 子仓提交先 push，再推进主仓 pin。
- 新 profiler SDK 在参考 Foundation 中已经是 `third_party/profiler_sdk` 子仓，参考 `107a620a…`。保留原 `code.byted.org:spatial/profiler_sdk.git` 来源与访问边界；不要复制一份 SDK 到 shadPS4，或把私有源转发布到另一 remote。没有访问权限必须如实报告依赖未取得，不能用空实现过构建。
- 通用 ring/file/socket、生命周期安全的 Layer/context 管理、公共证据格式辅助下沉 Foundation；PS4 NID、PM4/GCN、ELF 身份、Session/Orbis 行为留在主仓。
- 本仓已有 Foundation DebugBus，但没有生产 app 调试 service/完整命令 registry。`FOUNDATION_DEBUGBUS_BUILD_PROFILER` 现在 OFF。增加最小可独立链接的 profiler target；避免为 ringbuffer 顺便引入整套网络/反射/旧模块单例。
- Android 当前强制 `TRACY_ENABLE=OFF`，原因是旧 pin 不适合 dlopen。**本轮首选新版 PROF ring/file/socket，让 litep 消费；不能直接解除此门控就声称完成。** 已有 Tracy zones 可通过门面选择性复用，保留 desktop；不得同一进程链接两份 Tracy/SDK runtime。
- 任何需要改动的子仓，修改前核对实际 remote/分支/规则及已有授权。保留无关 `references/Bachata-S4` 脏改动、`externals/dear_imgui/`。本阶段 FEX pin 不变。

## 3. 工作包 A：能控制、能看状态、能抓图和性能

### 3.1 一个控制入口和真实状态

建立 process-owned `DiagnosticsHub`，Session generation 通过可撤销引用挂入。Android main thread 的 `Service.dump` 只解析有限参数、读取快照或入队；**不得等待 Session mutex、VM drain、GPU fence、导出结束**。忙时返回 `busy/retryable`。命令必须在启动、运行、卡帧、Stop、重新启动时都可查询。

Debug service 不另起进程，不自行重建 emulator；复用 app 初始化、Foundation DumpsysBridge 与 registry。仅调试构建注册这些控制入口，限制为 app/adb shell 可调用，保持现有 APK 数据与文件授权。JNI 生命周期归 app，Foundation 不增加 JNI_OnLoad 或 Java worker。

建议沿用 Citron/现有工具兼容命令；命令内部共用类型化后端，ImGui 只调用相同后端：

```text
debug_status
renderdoc_status
renderdoc_capture [frames]
renderdoc_capture_status [request_id]
renderdoc_capture_cancel [request_id]
guest_screenshot [game|overlays]
guest_screenshot_status [request_id]
profiler_ring status|start|stop|dump [frames]
profiler_capture status|file|socket|stop [max_mib] [seconds]
performance_capture [frames] [max_mib] [rdc:on|off]
performance_capture_status [capture_id]
performance_capture_cancel [capture_id]
guest_command_trace status|arm|save|cancel ...
gpu_command_trace status|arm|save|cancel ...
overlay status|show|hide
```

`profiler_ring` / `profiler_capture` **严格复用参考 Foundation 已有命令语法与字段**，不要另造 litep 私有协议。上面其他命令为本 spec 新接口，帮助和 schema 必须由实际 registry 导出。

- 所有异步任务返回 request/capture ID，状态至少有 armed、capturing、writing、ready、cancelled、failed；ready 必须同时有稳定文件和 sidecar。一次仅一个相关 GPU 捕获事务；重复请求返回 busy，不覆盖旧文件。
- 状态包含 run UUID、PID、session generation、build/driver identity、stage、最近推进时间、停止原因。状态快照不触碰 guest 任意指针。
- 把 accepted guest flip、PM4 consumed、host draw/dispatch、queue submit、可验证 GPU retire、host present、overlay redraw 分开计数。显示“最后推进距今多久”和当前 wait reason；缺失 GPU retire 能力写 unavailable，不能推断 0。
- 120 秒测试 Stop、用户 Stop、surface loss、guest fault、host crash 不能合并成“退出”。Stop 发起者、deadline、终态进入时间写证据。

### 3.2 RenderDoc/Turnip 完整路径

先修当前 loader：NOLOAD 命中也取得 `RENDERDOC_GetAPI`；函数指针与 API version 判定失败安全返回，禁止空调用/assert 中止。正确处理 Android 宏、`RTLD_DEFAULT` 和真实已加载库/namespace。

**API 可见不等于 Vulkan interception 生效。** RenderDoc layer 的注入配置必须发生在 Vulkan instance/device 创建前；证实 `DriverLease` 指向的 Turnip dispatch 链实际经过捕获 layer。禁止悄悄回落系统驱动或同时混用另一个 instance 的函数表。

- 把 RenderDoc library、API、Turnip handle、Vulkan instance/window 的使用期与 Session 绑定，VK 对象销毁前不卸库。控制状态由单一 owner 修改，跨线程只发送命令。
- 显式设置 API device/window；Vulkan 使用所用 `renderdoc_app.h` 定义的 `RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE`，**不是把 VkDevice 或 ANativeWindow 随意 cast 当 device**。
- 本仓 Liverpool 的旧 Start/End 区间仅按工作队列 drain；End 还在 OnSubmit/Flush 前。以统一捕获 coordinator 替代，确保目标工作真实录制/提交及相应 Presenter 合成均在区间内。处理异步 compute、多次 GNM submit、已经排队的下一帧；不允许“下一个 submit_done 就是一帧”的假设。
- 默认等下一条可证明完整的 flip/present 链，捕获 1 帧；支持少量连续帧。请求可以因没等到边界而 timeout。不得为了生成 RDC 虚构帧边界。另有明确标注的 time-window/partial 捕获用于卡死无帧，不能算完整帧验收。
- 开始/结束必须检查 API 实际状态/返回值。Stop、surface/device 变化时有限取消或 discard；不持 VM/presenter锁等待导出，不延长原有 Stop 等待形成死锁。
- 捕获数变化、GetCapture 返回路径、设备文件大小/hash、同一 run/capture UUID 与 layer身份组成 receipt。文件 `.partial` 及失败记录保留，不能从“文件非空”推定捕获成功。
- 匹配 RenderDoc-for-Pico 或已验证的 standard-remote 发行包，**一次选定一套 host/server/layer/ABI**。工具能力需要实际验证；当前 MCP capture_launch 曾返回 unsupported，必须有 app-owned capture + 配对 helper 的可用启动路线。不得通过改工具名字重试模拟成功。
- 捕获前记录原 GPU debug settings、server状态；正常、失败、取消都恢复原值，清理本次 forward/server ownership，不清 app 数据、不自动卸载旧 server。不要在初次 RenderDoc 路线同时开启 validation layers。
- Android/Adreno RDC 使用匹配 Android/Turnip remote replay；macOS 用离线 DB/导出图分析。输出至少事件、pipeline、shader、绑定资源和一张显式选择的 texture 图，记录 replay 是否成功；capture success/replay failure 分开。

截图复用现有 Presenter 的 game-only / overlays readback，并增加异步 receipt：request_id、run/session、guest_flip_id、host_present_id、图像阶段、尺寸/format/colorspace、readback完成时间与 hash。game-only 应在 UI/合成后处理边界按明确命名取得；不要把前处理图说成最终显示图。GPU copy/fence 完成后再编码，slot 延迟回收。冷启动/no-frame 请求 bounded timeout；adb screencap 仅辅助显示观察。

### 3.3 litep：真实 PROF、阶段与等待

将参考 Foundation ProfilerRing / SDK、共享命令与 `Common::Profiler` 门面接入。进程只能一个 SDK owner，在 Android user paths ready 后显式初始化；禁止 DSO 构造时启动线程。Session 重启不重建进程 owner，只切换 session identity、注册/注销线程并闭合或标为 incomplete 的 span。

提供三种模式：

- 常驻 bounded ring，建议每 live producer 1MiB；内存预算必须计算线程上限，不能按“预计几条线程”无限扩容。正常运行不做文件写入。
- 连续 file，默认 32–64MiB / 10–30秒；遵守 SDK 参数上限和 litep importer 实际限制。
- socket Streaming，通过 loopback + 本次拥有的 adb forward；正确处理 disconnect、限时限量、Stop 后恢复原 ring 模式。

导出在 worker，ready 在文件/sidecar 均完成后发布；记录 SDK exact revision、模式、frame owner、requested/actual recording、retained ranges、overwrites/drops、partial endpoints。ring 不保证保留已退出线程，也不是 crash recorder；故障诊断要尽可能在 worker teardown 前异步导出，不能在 signal handler 调用 profiler。

litep 集成使用其 emulator-neutral `register_target(transport=android, package, service, serial, expected_pid)`、`android_ring/android_capture` 或 `capture_profile`、`collect_capture`、`open_file/open_trace`。**必须实际打开生成的 PROF 并获得非空 scope/线程/计数/完整性结果，不能仅检查 magic。** 不将 Citron 固定包名/端口拷进来。

打点优先覆盖真实关键链：

| 阶段 | 最低需要的时间/计数 |
| --- | --- |
| Guest/FEX | 每 owner Run、返回/暂停/取消、HLE invoke（NID/library/operation）、callback/WaitingHle；CPUState来源明确，不伪造逐指令热点 |
| VM/提交 | VM admission/锁等待、复制 DCB/CCB、submit enqueue、queue wait、PM4消费；等待与工作 scope 分开 |
| 渲染 | draw/dispatch、pipeline lookup/compile、SRT、shader翻译、buffer/texture upload/缓存命中 |
| Vulkan | command record、queue submit、fence/semaphore wait、retire、acquire结果、swapchain recreate、composite、present |
| 其他 | AvPlayer decode/guest callback、AAudio/AJM工作与等待，避免误把媒体等待归到 GPU |

同线程嵌套用 scope；跨 coroutine/线程/队列阶段用 region cookie。cookie/submit_id 显式随任务携带，不靠线程名、数组序号或相邻时间配对。记录 off→on generation，禁止旧 scope 在新 recording generation 结束。

Profiler SDK 的 source-frame 由**唯一串行 owner**在选定的真实 accepted flip 边界发出，并与 host present 单独建立映射；原始 frame=guest flip 不是经游戏证明的逻辑帧。不能每个 owner/overlay redraw 都 FrameStart，也不能用 heartbeat 伪造 guest FPS。首帧前/完全停帧时用明确的时间窗口数据和阶段快照；若 SDK 的 cold recording 不支持，能力中标明并提供独立 bounded lifecycle/Perfetto 路线，不给空 trace 编造帧。

GPU timestamp 单独实现并验证：检查 queue family timestampValidBits、timestampPeriod、计数回卷、query availability；每 queue 有 bounded query slots，fence/availability 之后再读，**不为取计时结果同步 waitIdle**。支持 calibration 时才做跨 CPU/GPU 对齐，否则只有局部 GPU duration。当前 Foundation ring 本身不提供 calibrated GPU track：第一版可输出与 submit/action IDs 关联的 `gpu-timing.v1` sidecar 和持续计数；litep 不支持原生导入时必须明确标识，不能把 CPU submit scope 叫 GPU duration。

分别量测 disabled、ring、file/socket、GPU timestamps 的开销；详细 draw/resource trace 与 RenderDoc 默认关闭。不能把捕获时的性能直接作为基准。参考 SDK Streaming 已有性能门槛失败，保留事实，不声称零开销。

### 3.4 ImGui Layer 迁移

复用 Foundation `Layer/LayerManager` 的 UI/context/输入机制，把 reusable 修复放 Foundation；本仓保留 PS4 调试面板内容、Session action 和 Vulkan资源适配。参考 Citron NativeLayer / StatusLayerRenderer 的职责拆分，不复制 System 单例、OpenXR layer 或整套renderer。

**当前游戏输出依赖 `ImGui::Image` 合成。** 迁移不是删除旧 NewFrame/Render 再画个 HUD；必须保留游戏纹理、缩放、后处理、HDR/SDR与 screenshot 阶段。新 overlay pass 不得用 clear 抹掉游戏图；若继续在同一 render pass 合成，要验证 game layer 始终存在和正确顺序。

- 先统一一个 Dear ImGui 源码 target/版本/config：当前主仓1.93.0 WIP、Foundation1.92.2b，未跟踪 dear_imgui 是另一份旧树，禁止采用。为 Foundation 增加 external ImGui target 注入或兼容适配；不能跨版本传 `ImDrawData` / `ImTextureID`，不能同一 DSO 链两份 ImGui。
- Vulkan backend 消费实际所用版本的动态 texture/font create/update/destroy 协议，处理中文/英文/数字、DPI与resize。不要先开启 SDF 再假定有正确shader支持；基础 bitmap atlas 路线正确后再复用已有 SDF 能力。
- 明确 context owner；在 render thread 执行 `doFrame` / texture upload / Render，其他线程排队输入与command，传值快照而非悬空 ImGui raw pointers。Foundation 的 `getInstance()` 不作为新 Session 隐式owner。
- 旧 `ImGui::Layer` 对外接口可暂保留适配壳，但新增 manager/context 生命周期收敛到一个 owner；移除 raw-pointer 异步队列的跨 Session 悬空风险。desktop功能继续构建。
- Android Touch/KeyEvent/MotionEvent 从现有 NativePadBridge/Foundation input 分流，按 viewport/inset/DPI 映射，文本输入走 Android IME。一次明确的 capture token 决定事件归属；遮罩打开时释放 guest 已按键，关闭/失焦/设备断开/Stop时不遗留 stuck keys。不得重新引入 SDL。
- 最低面板：run/driver/build身份；各阶段推进计数与最近时间；帧时间与等待；RenderDoc/截图请求与结果；profiler模式/预算/导出；两种 trace 状态/损失；Stop。按钮均异步，可取消，可显示 failed。
- 在 guest 无新帧时可绘制缓存的最近图像和调试叠层，但此类 redraw 独立计数，不冒充 guest flip；若renderer本身不推进，adb/debug service 仍可读状态/取消，不能要求先点一个已经冻住的ImGui按钮。

## 4. 工作包 B：Guest 命令、PM4 与跨工具关联

### 4.1 一套身份和真实传递链

所有记录公共头至少包含 schema/version、run UUID、PID、session generation、capture UUID、clock域/单位、主仓/FEX/Foundation/SDK revisions、host/JNI Build IDs、title/version与guest模块hash、Turnip和RenderDoc身份、配置hash、截断原因。

关联 ID 建议全部序列化为字符串以保留 u64：

```text
run_uuid / session_generation / capture_uuid
context_id / guest_thread_id / invocation_id
submission_id / command_buffer_id / queue_id / queue_generation
packet_id / packet_offset / parent_ib_id / action_id
accepted_guest_flip_id / host_submit_id / host_present_id
resource_id / backing_generation / shader_hash / pipeline_hash
```

同进程重启、同 VA remap、队列复用都不能重用完整键。某字段不可得写 unknown+reason；不要用0伪造有效身份。一个 guest action 可以对应多个 host actions，异步compute可能跨flip，不能强行一一配对。

### 4.2 原始 PM4 / GPU 指令层

定义本仓独立子格式 `ps4.pm4.command`，建议后缀 `.gpu.pm4.trace`。复用 Citron recorder/异步writer/reader严格校验思路，**不复用其文件 magic、opcode或硬件地址结构**。

- 在 `guest_graphics_hle.cpp` 校验/复制 DCB/CCB 前收集 guest VA、完整 span、权限/backing generation、提交线程/HLE invocation、已确认 callsite。trace envelope 随 owned command copy 传入 GNM/Liverpool任务；不反查 host vector 指针当作 guest地址。
- 同时覆盖 graphics DCB、constant CCB、async compute ACB/queue、ring wrap/queue重映射、nested/chained IB；记录父子关系及进入/退出、完整 requested/captured byte ranges。对后者记录实际消费时的内容和版本，区分 enqueue snapshot 与 consume snapshot，不假定永不变化。
- 保留原始 DWORD、packet type/opcode/长度、寄存器写、draw/dispatch、DMA/copy、WAIT_REG_MEM、EOP/EOS/event/flush/同步及flip相关packet。未知合法opcode保留字节并标记unknown，不让trace改变正常执行；畸形长度/嵌套循环按独立有界规则拒绝或截断，不能越界读取。
- 不只检查首个页；跨页/alias/remap逐span描述。现有VM/GPU drain规则保留；trace不长时间持pin/VM锁，不为抓取开旁路MAP_FIXED或修改权限。
- GPU shader“指令”最低包括实际使用的 GCN shader 字节/静态反汇编、stage/entry/hash，以及生成 SPIR-V 的hash与pipeline关联，可按过滤器导出。**不声称获得硬件逐lane/wave动态执行 trace**；这与PM4命令流和静态ISA不同，硬件级shader执行追踪不属本轮。

容量同时受 frames、wall time、bytes、records、nested depth约束。建议默认8帧/64MiB/10秒，配置硬上限明确；disabled路径先检查atomic，不读取guest内存或分配。OOM/配额满只停止采集并标partial，不影响游戏线程。导出在worker、原子完成receipt，不在GPU热路径写JSON/SQLite。

### 4.3 decoded guest action / resource 层

定义 `ps4.guest-command`，建议 `.gcmdtrace.ps4`。在真正解码并决定 draw/dispatch/copy 的位置（Liverpool/Rasterizer/cache）记录，而不是把一次GNM提交直接当成一个draw。

每个action关联原始packet、submission、queue；记录关键graphics/compute state、render target/depth的mip/slice/format/layout、viewport/scissor、draw参数/index/indirect、shader/pipeline与buffer/image绑定。资源带完整 guest address spans、backing/copy/upload版本；CPU提交者不等于CPU写入者，未追踪到writer写unknown。跨coroutine队列保留显式context，不用全局“当前action”或只依赖thread-local。

选择性bytes probe有独立预算、resource selector和截取范围。不得默认导出全部guestRAM、所有纹理或整游戏代码。GPU写资源必须在相关GPU工作完成后读回；host缓存副本标出来源与时间，不能当作同一GPU时刻数据。

### 4.4 RenderDoc markers、profiler 与证据包

在实际 Vulkan command recording中按层次添加稳定namespace marker，例如：

```text
shadps4.guest {run,session,capture,submission,packet,action,queue}
shadps4.host {stage=upload|compile|composite|overlay|present,...}
```

要求同一command buffer内 begin/end正确嵌套；跨异步队列通过ID关联，不能把一个未闭合label迁移到另一个buffer。资源debug name包含稳定本地ID及hash，但不可依赖截断的人类可读字符串作为唯一权威键。

开发本仓离线decoder，至少具备 `format-check`、synthetic self-test、import SQLite、summary、action-index、按packet/action/resource查询。建议路径 `tools/ps4-gpu-trace/`；可复用Foundation基础工具，不在runtime引入C#/Python。

严格校验 endian、version、record size、offset/count溢出、duplicate IDs、未知record、长度截断和hash。原始字节可重取；解析不支持的record保持unknown，不丢弃后宣称完整。输出规范化 `foundation.gpu-action-index.v1`，namespace `shadps4.guest`。在公共容器/关联器中增加PS4子格式适配与测试，保留既有4种格式。若当前RenderDoc MCP不识别新namespace，用显式marker树适配产出等价结构并保留原event/parent，不能将其改名假扮Citron。

`performance_capture` 在同一未来边界协调PROF、两种trace、可选RDC和截图。已采样ring可以覆盖前史；必须记录各自实际覆盖时间和首尾边界，不声称所有payload都是同一瞬间。无新帧时支持 `dump retained + bounded time window + current wait state`，而不是无限等下一帧。

一个 `.gpusnapshot`/案件目录包含：

- manifest、capture request/receipt与版本/配置；
- PROF+SDK sidecar、可用GPU timing sidecar；
- `.gpu.pm4.trace`、`.gcmdtrace.ps4`、source hashes与decoder DB/action index；
- RDC及replay identity、marker/action映射；
- game-only/overlays图和显式present IDs；
- correlation报告：matched/candidate/ambiguous/unmatched、coverage、drops/partial、无法证明的字段。

复用Foundation ZIP64/hash/路径校验和不可覆盖输出。容器完整、marker匹配、同次运行、同帧覆盖、最终显示等效是不同结论，各自验收。截图/RDC/trace只保存到用户诊断目录，不进Git；Git仅放工具、synthetic fixture、精简验证摘要和hash。

## 5. 验收：一次交付，不以“已编译”收口

| 必须验证 | 通过条件 |
| --- | --- |
| 构建/依赖 | host .so/APK真链接；一个ImGui/SDK runtime；无SDL；子仓refs可取得、symbols与安装APK一致 |
| 控制寿命 | 无session、慢启动、GPU忙、导出中、Stop、连续三次synthetic重启都能快速响应status；旧generation请求不能作用新Session；不制造ANR |
| RenderDoc | ordinary APK +真实Turnip，至少一个有draw/dispatch的synthetic帧和一份TMNT捕获；对应remote replay可列event/资源并导出所选图；失败/取消恢复GPU debug settings |
| 截图/Layer | game-only与overlays有确切阶段/present receipt；开关面板不清屏；中英文/字体动态更新、resize、失焦/按键释放、Stop重启；游戏纹理仍合成 |
| PROF/litep | ring历史、file、socket各一份实际可读证据；同PID/SDK/frame owner校验；导出中Stop/断连接/冷帧/容量满/短命worker标识正确；无伪帧 |
| GPU计时 | 至少一组已完成query的duration和submit关联；availability、回卷与不支持calibration分支可测；没有CPU耗时冒充GPU结果 |
| 原始trace | DCB/CCB/ACB、nested IB、ring wrap、wait/event、跨页、unknown/truncated、cancel/预算；producer原字节与decoder一致 |
| action trace | draw/compute/copy、资源/GCN/SPIR-V/hash、guest→owned-copy地址身份不丢；同VA remap不串旧版本 |
| 关联 | 一个synthetic可从RDC action→guest action→PM4→submit/HLE来源，并能在PROF里找到同submit阶段；多host actions与跨队列允许明确多对一/unknown |
| 工具完整性 | corrupted/version mismatch/duplicate/partial/wrong PID/wrong build导入负例；容器verify和PS4 action-index匹配，不把partial变PASS |
| 性能 | 固定synthetic场景比较disabled与ring，建议目标中位frame/吞吐额外成本≤5%；记录p95/内存与样本窗口，测量不稳定写inconclusive；detailed trace/RenderDoc不套同一性能门槛 |
| 真实问题交付 | 一次TMNT正常片段或异常现场采集，能指出停在guest/HLE/PM4/shader/GPU等待/合成/present的哪一段，附可复查证据或明确缺失项；不得只交截图/总FPS/静态导入列表 |

实施时只跑这些改动的针对性测试和既有受影响的少量生命周期/输入测试，不重跑全V0/G2/全游戏回归。真实设备如不可用，保留阻塞和已完成部分，不用host测试替代普通APK acceptance。

## 6. 实施顺序与完成报告

**工作包A**先把Foundation最小依赖/单一ImGui目标、DiagnosticsHub、SDK owner和控制入口贯通，再接Layer、RenderDoc/截图与真实profile。**工作包B**补submit envelope、PM4/action record、markers、decoder与统一capture bundle；身份结构应在A开始时确定，避免到B再返工接口。

按风险并行组织代码可以，但不要创建用户未要求的新任务或让多个执行者同时修改同一子仓。完成后先push子仓，再commit/push主仓；更新AGENTS/CLAUDE/docs索引的事实状态，保留旧证据。

最终报告必须列：实际提交/子仓pin、APK与driver/SDK identities、命令示例、采集/导出/分析文件位置、上述验收结果、已证实的TMNT阶段瓶颈、仍无法判断的部分。**auto tag、FEX探针、逐条guest CPU trace保持延期，不得列为本阶段缺口或完成条件。**
