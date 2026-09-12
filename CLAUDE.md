# shadPS4 Android / FEX development context

@AGENTS.md

`AGENTS.md` is the shared project context and working guidance. Read it first; this file provides the Claude entry point without maintaining a second independent policy.

## 最新整版入口（2026-09-12）

先读 [Vulkan/NDK 复核与本地修复](docs/validation/android-native-host/vulkan-review-2026-09-12.md)，继续 [PKG v2 整版任务书](docs/specs/android-native-host-pkg-v2.md)。图形代码/测试/脚本已提交 `091334d3`，入口同步为 `1d411955`；新增 COMMON/bionic 适配 `06bd43bd`、进度 `7c675280`，本轮正在交付远端并独立复核。图形完整源列表37+67已生成104个ARM64对象，acquire合同19/0、SessionCore807/0；不是整个host链接或游戏验收。

**用户最新指定默认 Android/bionic Turnip，系统驱动适配后置。** 实際 native Turnip loader/dispatcher 尚未接入，不能只修改环境变量/枚举就宣布完成；锁定 bionic 包/ELF身份并查实际 shaderInt64，旧 glibc EMULATOR.zip 不能用于 native host。系统 Adreno 的 vkjson 仅为参考，不是 Turnip 结果。

下一 AI 连续完成正式host全链接+Turnip实际加载→完整guest/Orbis/VM/线程/Surface/音频输入→UI导入TMNT本体及更新/真机可交互/十分钟/Stop/同进程重启。不要重复修已关闭的SpinLock/platform/include/sweep/acquire局部缺陷，也不再按每个小gate停工。ANativeWindow寿命、完整WSI退出、allocator、信号/guest ABI等整版合同仍未关闭。

TMNT1.08是更新，匹配1.00本体已核实，路径/hash见 [pkg-set.json](docs/validation/android-native-host/2026-09-12-review/pkg-set.json)。以下早期记录仅作历史参考；事实冲突以 AGENTS 和最新复核为准。

## 历史入口与通用资料

- **历史执行：[host 原生化评估](docs/android-native-host-assessment-2026-09-11.md) → [host-native v1 spec](docs/specs/android-native-host-v1.md) → [进度](docs/validation/round2/progress.md)**。基点 `9ac6c300`；HN0 已落地 4 提交(`5d9edb92` HN0.1/HN0.3、`d6fb4df3` HN0.2、`e7195f0d` HN0 证据、`d4ea078e` HN1.1 seam)，均本机验证、未 push。**HN0.1 session 生命周期重构完成**：新 `src/core/host_runtime/`（backend-free `SessionCore` + `ISessionBackend` seam + `FexSessionBackend` + test gate），修掉 UAF/early-Stop 丢失/join 竞争/GuestFault 当 exit0；JNI 变薄(generation API、每出口 try/catch)；Kotlin 加 Ready/Stopping+generation guard。host `session_lifecycle_tests` 767 checks×20 run 全过(内建 alive UAF tripwire，本沙箱 ASan/TSan 不可用)；JVM 92/0；runner 23/23；arm64 `.so` 经 NDK29+FEX 构建成功、新 9-method JNI surface 导出。**HN1.1**：`RasterizerHooks` seam 让 `core/memory.cpp` 不再 include `vk_rasterizer.h`(headless 闭包唯一 blocker 解除)。仍未接主仓 loader/Orbis HLE/renderer。不要继续用占位游戏记录或固定循环宣称游戏整合。
- **关键限制**：（HN0.1 已修，勿再当原样缺陷）JNI 已无裸 context、无 join 竞争、GuestFault→Failed。仍未解：allocator pre-owned-region provider **BLOCKED**(FEX public 只有 self-steal `SetupHooks`，`Create64BitAllocatorWithRegions` 非 public，pre-steal 与 public SetupHooks 不兼容；见 `docs/validation/android-native-host/allocator-provider.md`)——已修 `call_once` 永久吞错。旧 VMM `address_space.cpp` USER_MIN=64GiB/USER_MAX=85TiB 与 guest_cpu `1<<36` 冲突、`module.cpp:104`/`linker.cpp` 把 guest 入口当 native 指针，都属 HN2 未做。**环境阻塞**：desktop 全量 build 在本 Mac 编不过(Vulkan-Hpp `eMesaKosmickrisp` + libc++ `stop_token`/`jthread`，未改文件同样报，项目在 Linux/CI 构建)，故 HN1 target-split 与 HN2 需 Linux/CI；device `9c2841a4`(AYN Thor/API33/4KiB) 用于 HN0 真机验收——**HN-U01 已 PASS(2026-09-12)**:UI 驱动 10 次 start/stop、generation 严格递增 gen 3→11、进程全程存活无崩溃(见 `docs/validation/android-native-host/hn0-device-u01-2026-09-12.md`),同时验证了 HN0.2 retriable guard;HN-S01/S02 设备矩阵、HN-A01、HN2 的 HN-L01 仍欠。Linux 无 public regions-taking SetupHooks，不误用 Windows HookPtrs，不擅改 FEX 子仓(pin `385a0cc4`)。完整 E1/E2/E3/H3、HLE 归属和 Swan G4 仍需验证。
- [一周目提交与全量欠项](docs/baselines/2026-09-08-round1-closeout.md)：`85b57cb2` 已推送；Swan contract 34/34、guest 45/45。历史 runner 的 11 PASS 不等于 app 验收，尤其 B02 仅有独立 ELF 证据。
- [事务加固与验证](docs/validation/v0/transaction-hardening-2026-09-08.md)：源码/hash/原始日志；测试时 dirty patch 已纳入上述提交，不倒改历史测试身份。

- [子仓归属与开发分支](docs/subrepository-ownership.md)：动依赖前核对自有 remote、分支和固定版本。
- [Foundation 接入](docs/foundation-integration.md)：DebugBus 已有构建入口；反射、packing、网络优先复用，完整闭包仍待验证。
- [基础版本状态：2026-09-07 / a7128893](docs/baselines/2026-09-07-android-fex-foundation.md)：固定起点、已验证结果、未完成项和恢复方法。
- [V0 总 spec](docs/specs/android-fex-v0.md)：完整目标及 CPU API/验收矩阵；二周目只是其子集。
- [较早的发布失败保护与执行准入](docs/validation/v0/followup-poison-2026-09-08.md)：历史记录，后续增量修复及当前成绩以一周目结项为准。构建入口统一为 `cmake/fex`（`-DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR=…`）。

- [研究索引](docs/README.md)：整体方案、Android 基础、FEX/Dynarmic、guest debugger 与 LLDB。
- [references 源码索引](references/README.md)：用途、固定提交、初始化方法。
- [Android / ARM64 整合审计](docs/android-arm64-integration-audit.md)：后续开发首先引用这份。

## 必须记住

- 当前目标是 Swan / Android 16 / ARM64 / **4 KiB**；用户于 2026-09-08 将 16 KiB 工作后置。FEXCore 只执行 PS4 x86 guest，shadPS4 host 保持原生 ARM64。
- Android 前端与 ARM64 HLE/guest 桥有可复用代码；当前有 NDK/bionic harness 和主仓 JNI CPU 冒烟 APK，完整 host APK 尚未整合。使用 FEX `385a0cc4d…`，不要按旧初稿回退至 reference 的旧 runtime pin。
- 第一阶段关注 NDK/bionic、4 KiB 设备上的真实 FEX 执行、原生 Vulkan Surface 和停止/重启生命周期；普通手柄接通不等于 PSVR/Move 支持。
- 调试先落地 host LLDB + guest 状态适配；异步 JIT stop 不能直接把 CPUState 当完整寄存器快照。
- 子仓提交、主仓 gitlink、部署 binary Build ID 是三个不同对象。记录和核对实际用到的版本；保留子仓中的独立未提交工作。
- Foundation 不替代 guest CPU API；网络命令投递给 owner thread，停用服务后等待 in-flight 请求退出再销毁 registry。不要将通用反射、序列化或网络设施在主仓重复实现。
- **测试用 PKG**：`/Users/bytedance/game/ps4/TMNT.Splintered.Fate_CUSA50828_v1.08.pkg`（CUSA50828 v1.08，1.87 GB）——后续 host loader/PKG 导入/真机运行阶段（HN2 真 ELF 起、HN6 PKG 导入）的测试内容。不提交进仓库。
- **host NDK/bionic 编译已起步**（`docs/validation/android-native-host/ndk-host-closure-2026-09-12.md`）：loader/memory/kernel-min 17/17 TU 过 NDK 交叉编译；修了 bionic 两处缺口——`time.cpp` 用 `date` 库+`USE_OS_TZDB=1` 代 `std::chrono::current_zone`，`kernel.cpp` 用 `arc4random_buf` 代 libuuid。新增 `AAudioOut`（阻塞写模型，无 callback，参考 citron）与 vk_platform Android surface 分支（`ANativeWindow*`→`vkCreateAndroidSurfaceKHR`）。bounded acquire、swapchain 生命周期、`CreateSurface` 去 SDL 耦合留 HN4。音频/Vulkan 参考本地 azahar/citron；foundation 仅 DebugBus 可复用。
