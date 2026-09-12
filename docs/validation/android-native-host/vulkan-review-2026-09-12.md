# Vulkan/NDK 复核、修复与后续交接（2026-09-12）

**图形源码的 Android 可移植性已有很好的证据。本轮修复后，root CMake 列出的 video_core 37 TU＋shader_recompiler 67 TU，全部用 NDK 生成了 ARM64 ELF 对象文件。尚未完成整个 host `.so` 链接、Turnip native 加载或游戏呈现。** 用户最新指定默认使用 **Android/bionic Turnip**；后续整版不先做系统驱动兼容。

被审主仓 `fd3587fd4dff7b20f5d30355bf0b5f165de053cc`，分支 `codex/android-fex-round2`。本轮修复留在工作区，未 commit/push；FEX `385a0cc4`、Foundation 及其他子仓未修改。原有 dirty/untracked 研究材料保留。执行入口仍为 [PKG v2 整版任务书](../../specs/android-native-host-pkg-v2.md)，本文是增量复核与修复记录，不另建微阶段任务书。

## 1. 对本轮汇报的判断

- `982cf0db` 的统一 teardown、owner 异常处理、control RAII lease、迟到 drain 留守确有实现；本次新运行 `session_lifecycle_tests` **807 checks / 0 failures**。不再把旧的 Prepare/Run 普通异常 abort 等反例描述为原样未修。generation-specific finalize、整个线程入口的异常兜底、析构 deadline 之后的所有权等完整要求仍以任务书 §4.1 为准；807 checks 不替代其余未覆盖合同。
- `fe6f8fca` 确实移除了 Service 的十秒运行寿命，改成 terminal 轮询和异步 onDestroy。116 个 JVM 测试是该提交作者的结果，本轮没有重跑或将其计作长寿命 Service 的真机证据；Stop receipt 仍未被消费、phase timeout/旧 observer/未退出终态需随生产生命周期闭环。
- `9b9ac50e`/`fd3587fd` 的选定 TU 编译具有价值，但当时只抽查部分图形源码且有失败，不能据此写“整个 host 闭包已编译”。`eMesaKosmickrisp` 确为 desktop 系统头路径问题；[上一轮复核](full-pkg-review-2026-09-12.md) 已区分 Darwin NDK 链接与 desktop 错误，不应继续把 macOS 本身当作 Android 构建阻塞。

## 2. 独立验证及直接修复

先用项目的 pinned Vulkan headers、实际 host-shader 生成器、ImGui 配置，并保持 Tracy OFF（未定义 `TRACY_ENABLE`），完整遍历 root CMake 的两个源列表。修改生产源前结果是 **97 PASS / 7 FAIL**，不是报告里暗示的“只有两处漏 define”。

| 原始缺口 | 来源与本轮处理 |
|---|---|
| buffer_cache、page_manager 缺 `Common::SpinLock` | bionic 定义 Unix/Linux，但没有 GNU `PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP`。原代码按平台 include AdaptiveMutex，再按功能宏选择 SpinLock，缺少后者声明。两处补齐真实 fallback include；不伪造 glibc 宏、不强制开启 Tracy。 |
| vk_platform 意外引用 X11 | 探针没有 CMake 的 `ANDROID` 宏，NDK 自带 `__ANDROID__`。源代码现在同时识别两者，避免 NDK 独立构建落进 Xlib/Wayland 分支；重复 Android Vulkan 宏有 guard。 |
| vk_shader_util 缺 GlslangToSpv | glslang 源码 target 与 installed package 的公开头路径不同。兼容两种实际布局，不引入系统版 glslang 掩盖 pinned dependency。 |
| cache_storage 缺 miniz_export | 这是生成依赖。新的工具通过 miniz 自身 CMake＋NDK 配置生成 export header，不手写伪头。 |
| 两个 shader TU 引用 src/common/types | 改成项目正常的 `common/types.h` include，不依赖额外暴露仓库根目录。 |
| vk_presenter 的 ImVec2 构造 | 正式配置需要 `IMGUI_USER_CONFIG="imgui/imgui_config.h"`，记录在新工具的显式 flags 中；未更改 ImGui ABI。 |
| 两份 sweep 不可复现且吞失败 | 原脚本仍读 `/tmp/host_inc.txt`、硬编码本机路径，传不存在 TU 会打印 fail=1 却 **exit 0**。现为兼容 wrapper，委托 `scripts/android/check-host-ndk-sources.py`；读取仓库内 include 集、保存每个命令/退出码/日志、失败返回非零，默认图形集来自 root CMake。 |

本轮还直接修正了 WSI acquire 的局部控制流程：

1. 每次 `AcquireNextImage` 至多一次 driver acquire，使用 100ms 参数；Timeout/NotReady 返回外层 render loop，不在内部无限重试。这个超时参数不证明驱动卡死时的严格墙钟上界。
2. Acquired/NotReady/Cancelled/Recreate/Error 明确区分；timeout/cancel 跳帧而不触发 swapchain 重建。
3. `VK_SUBOPTIMAL_KHR` 表示已经取得图像、acquire semaphore 已参与成功获取流程，先完成本次提交/呈现，再安排重建。Stop 恰好与成功 acquire 重叠也必须消费该图像，不能提前丢弃。
4. Presenter 暴露 `RequestStop()` 供生产 session 在 join 前调用，进入呈现和决定重建前检查 stop。**尚未替代最终 session/ANativeWindow ownership 接线**；`waitIdle`、fence/scheduler waits、DeviceLost 错误上报和 Surface generation 仍待整版处理。

### 本轮新验证

| 验证 | 结果 / 边界 |
|---|---|
| 修改前全图形 syntax sweep | 97/104，通过/失败源及全部命令已留证 |
| 编译修复后 syntax sweep | 104/104；随后 acquire 改动包含在下面最终对象构建中 |
| 最终固定源码 `-fPIC -c` | **104/104**；逐个检查 ELF magic、ET_REL、EM_AARCH64；不是用 `touch` 或空库冒充对象 |
| acquire 合同测试 | **19 checks / 0 failures**；真实生产 helper＋fake driver 回调，验证单次调用、停止前/中、Suboptimal、丢失 Surface 与设备错误；不是真 GPU WSI 测试 |
| SessionCore 回归 | **807 checks / 0 failures**；本机一次新运行 |
| 两个旧入口的负例 | 从 `/tmp` 启动并传不存在 TU，均 `pass=0 fail=1`、**exit 1** |

新工具用 API33、NDK `29.0.14206865` 的实际 Clang21、C++23，未启用 Tracy/userfaultfd。它是明确限定的源文件检查工具，不是正式 host target；完整 API/STL/依赖闭包仍须由 Android CMake 固化。对象文件仅在忽略的 build 目录；[证据目录](2026-09-12-vulkan-review/) 保存源码 hashes、命令、结果、对象 hashes、差异和原始日志。源码运行前后做一致性检查，防止混用改动前后的文件。

## 3. Turnip 主线及实际设备能力的边界

用户于本轮明确：**默认先用 Turnip，系统驱动适配后置**。据此不在当前批次实现为系统驱动补 Int64 降级，也不自动引回 Vortek/glibc。

本次只读 `adb -s 9c2841a4 shell cmd gpu vkjson` 取得的是 **AYN Thor / API33 / 4KiB / 系统 Adreno 740 驱动**：Vulkan **1.3.128**，swapchain、push_descriptor、vertex_attribute_divisor、robustness2 四个硬性扩展均有；shaderInt64=false。此信息说明系统 Vulkan 基础并不薄弱，但**不是 Turnip 的能力结果，也不是 app 内呈现验收**。

当前 guest SPIR-V backend 的 `SetupCapabilities` 无条件声明 Int64，fault-buffer host shader 也使用 uint64；`Profile::support_int64` 字段存在却没有对应完整降级路径。不能通过删 capability 或谎报 feature 解决。Turnip 可能提供需要的能力，必须以实际选中的 bionic 驱动版本查证，不用系统 vkjson 或其他设备的 Turnip 结果代替。

已定位下一 AI 必须一起改的加载边界：

- `RuntimeProfileResolver` 的 null 默认当前仍落到 system；旧 bundled `*-EMULATOR.zip`/某些枚举指向 glibc，不能把 DEFAULT 改成这个名字就称接好 bionic Turnip。
- `VulkanDriverConfiguration` 只形成 `BACHATA_VULKAN_*`/`SDL_VULKAN_LIBRARY` 环境字典，当前 CPU smoke session 不消费它们。
- `vk_platform.cpp::CreateInstance` 使用默认 `vk::detail::DynamicLoader`；新的 host 接入必须把所选 bionic Turnip 的真实 `vkGetInstanceProcAddr` 传给 dispatcher，持有 loader/namespace/库寿命，并记录所加载 ELF 的 SHA/Build ID。
- 缺默认包或不兼容时明确失败，不自动退回系统；在实际 Turnip 上检查 Vulkan1.3、shaderInt64、required extensions、feature bits/formats，并记录 driverName/driverInfo。确认成功后再让 renderer 构造缓存/pipelines。

本轮未下载/安装新驱动，未改变其他 app/系统 GPU 配置，未启动游戏。Turnip 已成为**执行任务书的默认策略**，实际原生加载的完成状态仍为未接入。

## 4. 下一 AI 继续做一个完整版本

[PKG v2](../../specs/android-native-host-pkg-v2.md) 已更新 Turnip 策略和本轮修复状态。连续完成以下三个内部工作包，不以其中任一步单独结束交付：

1. **构建＋Turnip 加载**：使用真实源列表/依赖 target 形成 host `.so`，`--no-undefined`、API33 auxiliary 与 Swan profile 身份一致；将 Turnip 默认选择、安装 ABI 校验、adrenotools/native loader、dispatcher 和错误上报接通。现有 104 对象能复用图形编译结论，不能省掉剩余 COMMON/全 HLE/媒体/输入依赖的编译和链接。比如 `common/signal_context.cpp` 仍缺 Android ARM64 分支；本轮 page_manager 编译通过并不证明其调用的信号实现已可链接。
2. **生产执行与资源所有权**：统一 guest VM/allocator、真实 loader/module entry、typed Orbis HLE、线程/TLS/两层回调，连接 CPU/GPU memory observer；完成 ANativeWindow generation、Stop/detach→Presenter::RequestStop→GPU/worker 退休、输入和 AAudio。遇到错误定位修复，不能用 dummy RegisterLib、忽略 undefined、固定循环替代。
3. **真实 PKG 真机验收**：按已核实 manifest 安装 TMNT 1.00本体＋1.08更新，通过普通 UI 实际启动；真实游戏帧与输入/音频、可交互场景、连续十分钟、Stop＋同进程三次重启。AYN 与 Swan 分开记结果。没有满足则记录最后实际失败 stage/NID/RIP/driver，不将对象编译或首帧当作整版完成。

16KiB、VR、finite Step 继续后置。当前进步显著降低的是**图形源代码移植风险**；正式构建闭包、FEX/Orbis/VM 接口和 Android 图形生命周期仍是实际落地工作。
