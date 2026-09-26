# Spatial Foundation 接入记录与 V0 复用要求

日期：2026-09-07。这是初始基线 `a7128893` 之后的依赖准备里程碑；不改写初始基线记录。
主仓原先只有桌面核心，本次增加基础设施的构建入口，尚未提供 Android app 或 guest backend。

## 2026-09-26：日志迁移到 Foundation LogModule，移除 spdlog

`common/logging` 的后端从 spdlog 换成 Foundation 的 `LogModule`（`GLOG`）；`LOG_*` 宏与调用点不变。

- **类别 = Foundation logger kind。** 每个 `Common::Log::Class` 注册为一个以类名命名的
  `spatial::ILogger`（如 `Kernel.Vmm`、`Lib.SharePlay`），类别级别就是 kind 开关；
  `Log.filter`（`*:info Kernel.Vmm:warning`）、Foundation 的 `ILogger::SetLogLevelByName`
  与 DebugBus `log_filter` 改的是同一个开关。Foundation 模块自己的 kind（如 FDM 的
  `FoveationVulkan`）也在同一张表里。日志不做限流：刷屏靠类别开关治理。
- **异步通道 + 轮转文件。** 主通道（线程 `shadPS4:Log`）写 Foundation `RotateFileWriter`，文件名
  取原名去扩展名再加 `.log`：`shadps4.log`（启动）、`shad_log.log`（游戏，原 `shad_log.txt`）、
  `<serial>.log`（`separate`）、Android `android-host.log`。打开时旧文件轮转为 `<name>_1.log`、
  `<name>_2.log`（保留上一轮），超过 `size_limit` 同样轮转。guest patch 探针是独立的
  `LogModule` 实例与通道，仍写 `guest-patch.log`，不进控制台。Critical（断言）行写完即 flush；
  `flush_level` 按级别在写后等待落盘（每行一次通道往返，只在排查崩溃时设低）。
  桌面控制台用 Foundation `ConsoleWriter`；Android 不写 logcat。
- **计数与查询。** 每 250 ms 向 Litep ring 发累计 counter `Log.Lines`、`Log.MessageBytes`、
  `Log.Class.<name>`；DebugBus `log_stats status [top]` 列出按近 10 s 速率/累计行数排序的调用点
  （file:line）与类别，`log_filter [<kind>:<level> ...]` 运行期替换过滤器并列出所有 kind 级别。
- **设置清理。** 删除只对 spdlog 有意义的 `sync`、`skip_duplicate`、`max_skip_duration`、`type`
  （含 big picture 设置页、Android 设置目录与兼容配置写入）；旧配置里的这些键被忽略。
- **Foundation 侧。** 新增 `modules/log`（`spatial::foundation_log`）：已加入 basic 层的桌面宿主上
  是 `foundation_core_minimal` + `foundation_module_log`；Android 编译只含日志源码的窄版本，
  不带 mimalloc（Foundation 在 Android 上为它设置 `MI_TLS_SLOT=2`）、模块运行时和 JNI helper，
  文件目录须为绝对路径。另修：`getRotateFileWriter` 在 POSIX 上把绝对目录拼到可写目录下；
  Windows 控制台不再改终端字体/缓冲区/输入模式（原先关掉了 Ctrl+C 处理），换行不再重复。
- 验证：桌面 MHW 运行中 `log_stats` 定位到 Kernel.Vmm 三个调用点各约 90 行/s，
  `log_filter *:info Kernel.Vmm:warning` 后立即静默；ring dump 含 `Log.*` counter；正常关窗后
  控制台与文件末行一致；`shadps4_settings_test` 74/74；Android host `HOST_LINK_PASS`，ELF 无
  `STATIC_TLS`；Kotlin `ShadPs4ConfigManagerTest`/`RuntimeSettingCatalogTest` 通过。
  未做 Android 真机运行验证。

## 2026-09-26：桌面材质 Medium 的 BC7 重编码、帧颜色与状态层交互

- Texture quality Medium（0.75 倍）在只采样 BC、不支持 ASTC 的桌面 GPU 上，16 字节 BC 源块改用
  Foundation `texture_codec` 新增的 `Bc7Shader()`（mode 6）重编码；8 字节 BC1/BC4 保留原格式（转 BC7
  存储反而膨胀），支持 ASTC 的设备仍走 ASTC。`VideoCore::BlockCodec` 记录实际编码，view/诊断按它选择格式。
  独立 GPU 测试（硬件解码对照源图）在 RX 7600M XT 与 Radeon 890M 上各 74 checks / 0 failures。
  血源 Medium 实测约 60 次 BC7 分配（如 `1024x1024 Bc7UnormBlock -> 768x768 ... levels 11 -> 10 mode BC7`）。
  首轮 Medium（RX 7600M XT）出现一次 swapchain acquire `ErrorDeviceLost`，日志保留在本机运行目录
  `user/log/shad_log-bc7-devicelost.txt`（不入库）；之后开 crash diagnostic 约 10 分钟、普通复现约 20 分钟
  均未再现，原因未定，不归因 BC7。
- VideoOut `A8R8G8B8Srgb` 帧（字节序 B,G,R,A）在 presenter 用 R/B component swizzle 采样，截图读回用
  BGRA；不做 B8 格式重解释，不增加图像或内存。血瓶/HP 条恢复红色。
- 状态层：Only FPS 条/Summary 标题统一控制 Detail 与 Controls，Detail 标题行与 Controls 相同、宽度有上限，
  FPS 位置可配（桌面默认左下），Status/Detail 不抢键盘手柄焦点，Renderer 显示实际 GPU。操作见
  [状态层指南](guides/status-overlay.md)；Foundation overlay 测试 239 cases / 1833 assertions 通过。

## 2026-09-25：桌面 DebugBus TCP（Foundation `debugbus_tcp` + NetSystemModule）

桌面版编入与 Android dumpsys 相同的命令表（`diagnostics_commands`/`diagnostics_service`），
经 Foundation `spatial::foundation_debugbus_tcp` 在 `127.0.0.1:32124` 提供服务；不另写 socket。
[cmake/SpatialFoundation.cmake](../cmake/SpatialFoundation.cmake) 在非 Android 宿主加入真实的
`third_party`、`modules/property`、`basic`（core/allocator/async/imodules/network，libevent
为 Foundation 自带版本，fmt/Vulkan/VMA/zlib 绑定宿主 target），不再用同名空 target 冒充
network/profiler。Foundation 侧机制修正：`core_minimal` 不再硬依赖 Crypto++（`Crypto.cpp`
拆为可选的 `foundation_core_crypto`）；debugbus 的 tcp/profiler target 仅在依赖模块存在时声明；
`foundation_add_subdirectory` 与宿主依赖绑定可被只加部分层的宿主 include。Android 仍只用
registry + dumpsys，不构建 basic 层。

- 参数：`--debugbus-port N`（0 = 任意空闲端口）、`--no-debugbus`；`Emulator::Restart` 保留设置。
- 协议与 Azahar DebugDump 客户端兼容：连接后两行 greeting，每行一条命令，回复以 `--END--`
  行结束，`exit` 断开。handler 在 Foundation 网络线程执行，须只做线程安全的投递/查询。
- 客户端：`python scripts/debug/debugbus.py "overlay status"`；抓帧
  `python scripts/debug/debugbus.py --wait 300 "renderdoc_capture 1"` 等待到 ready/failed。
- RenderDoc（未安装、只有 MCP 自带包时）：不要用 `renderdoccmd capture` 注入——dll 进程内可见但
  Vulkan layer 未注册，StartFrameCapture 会失败。改用 layer 环境变量：
  `VK_ADD_LAYER_PATH=<renderdoc>\qrenderdoc VK_LOADER_LAYERS_ENABLE=VK_LAYER_RENDERDOC_Capture`。
  F12 与 DebugBus 共用同一 capture coordinator，结果写入 `user/captures`。
- `SHADPS4_VK_DISABLE_EXTENSIONS=ext[,ext]`（Android：`debug.shadps4.vk_disable_extensions`）
  把设备扩展视为不支持，用于在任意 GPU 上复现缺扩展驱动或 RenderDoc 下的回退路径。

## 2026-09-24：可选的 scrcpy 嵌入式录制 SDK

SDK 源码现由私有仓 [`tencentmalos/my_mcp_tools`](https://github.com/tencentmalos/my_mcp_tools)
的 `dev_tools/mcp/scrcpy/capture-sdk` 维护，`master` 首个提交为
[`890ce3d`](https://github.com/tencentmalos/my_mcp_tools/commit/890ce3df2e632b45317484247d95f2449655bbec)。
Foundation `main` 从
[`3c66847`](https://github.com/tencentmalos/foundation/commit/3c668471de11653682e2c68f9ecf8bfb784ced1b) 起提供可选的
`spatial::foundation_capture` target。Android 构建时从仓库根目录或 SDK `native`
目录解析源码；未指定 SDK 时不构建录制适配层，普通宿主构建不依赖私有仓权限。

```sh
git clone git@github.com:tencentmalos/my_mcp_tools.git /path/to/my_mcp_tools
python3 scripts/android/build-host-android --ndk /path/to/android-ndk --api 33 \
  --out /path/to/capture-host-build \
  --scrcpy-capture-sdk-root /path/to/my_mcp_tools
```

`capture_video start|start_live TOKEN|stop|status` 通过现有 DebugBus 控制，源是
`shadps4.final_render_target` 的完整 canvas，宿主 ImGui 状态层不进入录制画面。
SDK 工具用 `python3 /path/to/my_mcp_tools/dev_tools/mcp/scrcpy/capture-sdk/tools/capturectl.py`。
[AYN 实机首验与限制](validation/android-native-host/scrcpy-sdk-source-20260924.md)记录了既有结果；
立体分眼、样本与生产帧精确关联和持续录制性能尚未验收。

## 2026-09-24：Profiler SDK 源码集成

Foundation 现在跟踪共享 `main`。`third_party/profiler_sdk` 已从内网子模块改为
Foundation 直接跟踪的普通源码，SDK、配套 C++/Python reader、测试和规格一起保留。
迁移前先同步 SDK 上游 `main` 的 `f34a1c8df05d870bdfdd6c9d9f47e97b8963a8f6`，
并保留现有集成分支的 GPU metadata、reader 和无帧采集修复。
[版本与维护方式](../foundation/third_party/profiler_sdk.UPSTREAM.md)记录了完整树身份。

宿主的 `modules/profiler_ring` / `profiler-sdk` CMake 入口及分析器路径不变；
Foundation 适配了上游删除的 `bookmark_dyn`，保持 LiteTrace 对临时名称的复制语义。
新机器只需初始化 Foundation，不再需要内网 SDK 权限：

```sh
git submodule update --init foundation
```

已有 checkout 在更新父仓 gitlink 前应先保存 Foundation 和旧 SDK 的本地修改。
Git 可能把旧 SDK 的 `.git` 文件留在原目录；它不是新版本的依赖。新机器和干净
checkout 不含该嵌套 Git 元数据，不应再对 profiler_sdk 执行 submodule update。

验证：SDK Debug CTest **80/80**、Python analyzer **91/91**、C++/Python 采集导出
（123 chunks、0 skipped、无截断、Perfetto 字节一致）与 Release encoding 检查通过。
Foundation 的干净本地递归克隆没有 `.gitmodules` 或 SDK `.git`，SDK 目录树与同步结果
`c1204a4ae4b5202c5bb682e60609994030916995` 完全一致；仅允许 Git `file` 协议执行
递归初始化也成功，ring round-trip/file/socket 两项测试通过。

shadPS4 的实际 `shadps4_add_foundation()` profile 使用父仓固定的 fmt、Vulkan Headers、
Oboe target 做独立接入探针：macOS shared library + registry smoke **1/1**，Android
arm64-v8a/API 35（本机 NDK 29）包含 SDK/ring/audio/dumpsys 的 101 个构建步骤及
shared library 链接通过。探针复用 `tests/foundation` 的两个 C++ 文件，在外层先提供
上述真实 target；没有启用完整模拟器构建。Foundation input 单测通过；audio 单测在
AppleClang 17 的 `-fexperimental-library` 配置通过（默认 libc++ 未暴露 stop_token/jthread）。
本轮未安装 APK、操作设备或进行游戏回归。

SDK 最新 Bookmark wire 布局仍使用 PROF v3；新采集用随源码集成的 reader，旧采集
保留匹配旧版本的 reader。以下章节是早期里程碑，版本以当前 Foundation gitlink 为准。

## 来源与已经落地的范围

与 azahar 使用同一个 [tencentmalos/foundation](https://github.com/tencentmalos/foundation)，
从 azahar 干净 checkout 的 `b753063273a73ff6664c567f62cfa5070f5d60a5` 建立
`codex/shadps4-android-fex-v0`。在自有分支为 DebugBus 添加两个默认 ON 的 CMake 开关，
让小型宿主可关闭 TCP/profiler adapters；公共 C++ API 和 azahar 默认构建行为不变。
本次固定提交为 [`1f7008848736b7c6c0779220480344fd5b5fc5e3`](https://github.com/tencentmalos/foundation/commit/1f7008848736b7c6c0779220480344fd5b5fc5e3)。

- `foundation/` 是正式 **build dependency submodule**，不放在只读研究用 `references/`。
- 主仓 `ENABLE_SPATIAL_FOUNDATION=ON` 默认启用，通过
  [cmake/SpatialFoundation.cmake](../cmake/SpatialFoundation.cmake) 提供 `shadps4::foundation`。
- 当前目标实际链接 `spatial::foundation_debugbus`，Android 时再链接
  `spatial::foundation_debugbus_dumpsys`；静态库按 PIC 构建。
- 最小 profile 只加入 `foundation/modules/debugbus`，关闭 TCP/profiler target。
  没有引入全局 allocator、JobSystem、第二套 ImGui、XR 或 libevent。（2026-09-25 起桌面
  profile 为 DebugBus TCP 加入 basic 层与 libevent，见上文；Android 不变。）
- 主程序已有 link 接入；没有注册运行时命令、启动网络端口或添加 Kotlin Service。
  这一步提供可用构建基础，不代表应用侧功能已经上线。

选择这个入口的依据是 foundation 自己的
[宿主集成指南](../foundation/docs/guides/integrating-emulator-host.md) 和
[API 稳定性说明](../foundation/docs/guides/api-stability.md)。仅将完整 foundation
`EXCLUDE_FROM_ALL` 仍会解析全部 CMake，无法避免缺失依赖和全局选项副作用。

## 必须优先复用的能力

**2026-09-13 输入已接通：** owned `codex/shadps4-android-fex-v0`已push `5388ef45313d6c32cb5f4bb5b07f1246ee381370`。[Runtime/input复核](validation/android-native-host/runtime-input-review-2026-09-13.md)记录Foundation portable49/0、Android5 tests及主仓真实scePad/普通APK证据。根host单独加入 `modules/input` / `spatial::foundation_input`，Gradle单独引用其Android Kotlin library；通用设备/值事件/状态/反馈留Foundation，PS4用户/端口/ABI/历史、应用JNI/Session归属留主仓。无Foundation JNI_OnLoad/Java-calling C++ worker或OpenXR耦合；原[输入设计](specs/android-foundation-input-first.md)保留完整合同，自定义profile/物理震感/guest-origin验收尚未完成。本节以下2026-09-07成绩保持历史，不覆盖当前input证据。

下表后两列是 **待执行要求**，不能把目标名称存在当作本仓已完成集成。

| 能力 | Foundation 实现 | V0 的使用边界和后续工作 |
|---|---|---|
| 命令注册/诊断入口 | `spatial::foundation_debugbus`、`_dumpsys` | 已编译接入；V0 app 注册 status/capabilities/stop，写操作投递给 guest owner |
| 反射 | `spatial::foundation_meta_reflection`，`Builder.hpp` / `Objects.hpp` | 新的诊断 payload 优先复用；完成 allocator/core 依赖闭包后链接 |
| 结构化序列化 | `spatial::foundation_meta_packing` | 复用数据编码，定义版本/schema/非法输入行为；不另写通用反射或序列化框架 |
| 网络 | `spatial::foundation_module_network`、`spatial::foundation_debugbus_tcp` | 若 V0 启用远程控制必须复用；先验证 NetSystemModule 启停、libevent 和线程归属 |
| 数学、后续 XR | `spatial::foundation_math`、`spatial::foundation_xr` | 基础数学按需复用，PS4/PSVR 语义和渲染策略留在宿主；XR 不扩入 V0 |

Guest CPU 公共 API 继续使用自己的稳定值类型/opaque handle；不向它暴露 foundation
module 单例、反射元对象、FEX 类型或序列化布局。JSON 只是证据输出格式，不能因此要求
在 JIT 热路径反射 CPUState。PS4 `sceNet` 等 guest API 的语义也不由 host DebugBus 网络代替。

## 启用反射与网络之前必须解决的闭包

1. 反射链接 `foundation_core_minimal` 与 `foundation_allocator`；core 又依赖 `fmt`、
   `cryptopp`，allocator 依赖 mimalloc/property。当前主仓并没有提供完整 Crypto++ 接入。
   不能添加同名空 target 让配置表面通过。
2. 完整 foundation 还假设宿主存在 `vulkan-headers`、zlib 链接名 `z`，JobSystem 从
   `${CMAKE_SOURCE_DIR}/externals/boost` 取头文件。本仓使用不同的 target/目录，需要适配真实依赖；
   跨编译不能拾取 macOS/Homebrew 库。
3. `third_party/CMakeLists.txt` 对 Android mimalloc 定义 `MI_TLS_SLOT=2`、
   `MI_TLS_SLOT_FALLBACK=1`。V0 必须核对 ART、bionic、FEX 的 TLS 所有权，不直接照搬这个配置；
   当前最小 profile 完全不编译 mimalloc。这不是已经确认存在冲突，而是尚未验证的假设。
4. libevent 使用 `azahar_config/posix` 固定配置，包含 epoll 等假设，不等于所有 POSIX 平台通用。
   NetSystemModule/module system 的闭包要在 Android NDK 下真实链接并运行。
5. `TcpServer` 依赖已启动的 NetSystemModule，必须检查 `IsListening()`；网络线程不执行 guest
   控制逻辑。需要限长、取消/超时、断线、端口冲突及退出后的线程/FD 清理证据。
6. `DebugCommandRegistry` 注册表无并发修改保护；启动服务前完成注册。
   DumpsysBridge 解锁后使用非 owning registry 指针，`SetDumpsysRegistry(nullptr)` 本身
   不等待已进入的请求。宿主需要停止接收、解绑、等待 in-flight 请求退出，再销毁 registry。
7. 独立的 LLDB 稳定快照仍按 V0 API 契约实现；不能在信号处理器内调用 reflection、packing、
   DebugBus、JNI 或网络。Foundation 不解决 JIT 中 CPUState 的精确性问题。

一般机制的修正写回自有 foundation 分支。宿主只保留轻量 adapter；如果能力被真实依赖问题
阻断，记录具体问题和后续步骤，不在主仓再造一套通用网络/反射框架。V0 不要求远程 TCP，
先用进程内命令与 dumpsys 验证即可；启用 TCP 时必须补验收。

## 本次验证

| 检查 | 结果 | 边界 |
|---|---|---|
| macOS ARM64 / AppleClang 17 / Release | shared library 编译、链接，CTest **1/1 PASS** | 实际调用 foundation registry；不包含 FEX |
| Android NDK / arm64-v8a / API 35 / libc++ shared | registry + dumpsys + smoke shared library 编译、链接通过 | **未运行**，仅交叉构建 |
| ELF `PT_LOAD` | smoke `.so` 与该 NDK 的 `libc++_shared.so` 均为 `0x4000` 对齐 | 不是 APK ZIP 对齐或 16 KiB 实机测试 |
| API 36 配置 | 本地标记 r28c、r29 的包均失败：实际 max API 为 35 | M0 必须核验并取得真实支持所选 API 的工具链 |
| 完整 shadPS4 / reflection / network / Android app | **NOT_RUN / 尚未接入** | 不借用 azahar 的验证作为本仓结果 |

本地 NDK 的 `source.properties` 自报 `28.2.13676358` / `29.0.14206865`，但两者
`meta/platforms.json` 和 sysroot 都只到 35。因此记录的是这些本地安装的实际状态，
**不推断官方 r29 的能力**，也不修改安装目录来绕过检查。
[官方 r28 对齐说明](https://github.com/android/ndk/wiki/Changelog-r28) 和
[16 KiB 指南](https://developer.android.com/guide/practices/page-sizes) 仍需与真实产物核对。
原始产物摘要在 [构建记录](data/foundation-bootstrap-20260907.json)。

复现宿主检查：

```sh
git submodule update --init foundation
cmake -S tests/foundation -B /tmp/shadps4-foundation-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/shadps4-foundation-host
ctest --test-dir /tmp/shadps4-foundation-host --output-on-failure
```

复现本次受限的 Android 交叉构建（`ANDROID_NDK_ROOT` 指向实际安装路径）：

```sh
cmake -S tests/foundation -B /tmp/shadps4-foundation-android-api35 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DANDROID_STL=c++_shared -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
cmake --build /tmp/shadps4-foundation-android-api35
```

API 35 仅用于本次库构建探针；V0 的 Android 16 目标与验收要求没有降低。
后续 APK 仍需验证完整 native 依赖闭包、ZIP 对齐、真实页大小以及加载和退出。
