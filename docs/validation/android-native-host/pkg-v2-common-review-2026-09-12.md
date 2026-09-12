# PKG v2 最新进展复核与 Git 交付（2026-09-12）

**上一轮图形修复和遗漏资料已提交推送；本次复核发现的两个 COMMON 边界问题也已直接修复，并加入正式回归测试。AYN/API33 真机 GNU errno 7/7、POSIX errno 7/7、ARM64 信号上下文 50/50 通过。正式 host target、完整 `.so` 链接、Turnip native 加载和真实 PKG 运行尚未实现。下一步直接进入既定整版构建与整合，不再将更多单 TU 扫描当成独立交付阶段。**

执行仍依据 [PKG v2 整版任务书](../../specs/android-native-host-pkg-v2.md)，默认 Android/bionic Turnip，系统驱动兼容、16KiB、VR、finite Step 后置。

## 1. 已先完成 commit/push

被审工程基点 `7c675280`，分支 `codex/android-fex-round2`。上一轮修复已在 `091334d3`，入口同步在 `1d411955`；本次新增工程代码来自 `06bd43bd`，`7c675280` 只记录进度。

本次按用户要求，先将遗漏的本地资料归档，再通过 Git SSH 推送：

| 提交 | 内容 |
|---|---|
| `bb4d6c4a` | 遗留 Android/FEX specs、G2/G3/host 复核与原始证据，以及当前入口状态，524文件 |
| `2efe7004` | Windows Android/FEX前端/Vortek/Gladio研究资料，10文件，独立于工程修复归档 |

首次推送将 origin 从 `0e10defc` 推进到 `2efe700474c767bb36c4f19aecf557cd92297cd0`，包含此前51个本地提交和以上两个归档提交。已用 `git ls-remote` 复核；本轮 COMMON 修复、正式测试、报告和新验证证据随后一并提交推送，不倒改历史日志/源码身份。

规范地址：[tencentmalos/Bachata-S4](https://github.com/tencentmalos/Bachata-S4)，旧 `tencentmalos/shadPS4` remote 仍重定向到此。FEX、Foundation、Android/ARM64 reference 与 SG8275 reference 的固定 SHA 均通过远端分支可达性核验，未推进 gitlink。

仍保留在本机且不属于本次主仓交付的内容：`references/Bachata-S4` 两个旧 SG8275 修改（glibc X11 emulated-library 列表及 NDK29配置）和遗留 `externals/dear_imgui/`。没有把子仓 dirty 内容误称已随主仓保存。524份待归档文档已检查文件类型及压缩日志内容；未纳入游戏包、runtime二进制或凭证。原始txt/patch/inc证据中的空白保留，不为通过格式检查改写证据。

## 2. 修复前的独立验证（历史证据保留）

| 项目 | 本次结果与限制 |
|---|---|
| COMMON及调用者 | `signal_context.cpp`、`error.cpp`、`signals.cpp`、`page_manager.cpp` **4/4 ARM64对象编译成功**；NDK29/Clang21、API33、GNU默认profile |
| 实际运行 | 同一生产 `signal_context.cpp`/`error.cpp` 加独立探针，`--no-undefined` 链接，AYN Thor / Android13 / API33 / ARM64 / 4KiB **49 checks / 0 failures** |
| fault检查 | 20次真实PROT_NONE读异常、20次PROT_READ写异常，子进程SA_SIGINFO handler检查真实host PC和读/写分类；含PC/ESR正常链、短链、超长/零记录及errno字符串检查 |
| crypto/ipc | 分别加入实际libressl include与repo-root include，**2/2 syntax通过**；上一轮的路径归因现在有实际重跑支持 |
| 既有回归 | SessionCore807/0；acquire合同19/0；Python runner23/23 |
| 图形完整对象 | `091334d3` 已保存上一轮37+67=104个对象的证据；本轮没有把相同图形编译重复记成新增能力 |

[证据目录](2026-09-12-common-review/) 保存命令、源码/二进制SHA、对象编译记录、设备日志和额外负例。探针按shell UID运行，二进制本机与设备hash一致；不是普通APK/ART/FEX/GPU共同信号处理验证，没有安装驱动或运行游戏。探针成功证明当前COMMON函数能处理该设备真实异常，不证明整个renderer的fault恢复、缓存失效或guest归属已接通。

## 3. 本轮直接修复的两个边界（已关闭）

### strerror_r：按实际返回类型适配 GNU/POSIX

被审版本 `06bd43bd` 无条件将 `__ANDROID__` 加入 char* 分支，但 NDK `string.h` 实际按 `__USE_GNU && __ANDROID_API__ >= 23` 选择 char* 或 int。本次同一 NDK/API33 命令追加 `-U_GNU_SOURCE -D_POSIX_C_SOURCE=200809L` 后，确实出现 const char* 不能从 int 初始化的错误；保留修复前 [errno_posix.log](2026-09-12-common-review/errno_posix.log)。

已在 [error.cpp](../../../src/common/error.cpp) 用泛型 lambda 和 `if constexpr` 按 libc 实际声明的返回类型处理：POSIX 成功时使用调用者 buffer，GNU 使用返回指针；失败保留明确文本。Windows 分支保留。正式 [errno 回归](../../../tests/common/error_tests.cpp) 检查常见错误、未知错误和 GetLastErrorMsg；Android 的 GNU/POSIX 两个 target 均编译、链接并真机通过，macOS POSIX 路径也通过。该项不再交给后续 spec。

### ESR 链：遵循 16 字节记录布局

旧代码只按 `alignof(_aarch64_ctx)` 校验 size，约束的是头部的 4 字节对齐。12 字节的前置记录会被接受，下一条 esr_context 的 64 位字段不满足自然对齐。修复前设备反例打印 `misaligned_record_accepted=1`，单列观察，未计入旧探针 49 项 PASS；不能据此声称普通内核链已出现崩溃。

已在 [signal_context.cpp](../../../src/common/signal_context.cpp) 按 AArch64 UAPI 的 16 字节记录布局约束步长；保留每次剩余空间校验、终止头和未知记录跳过。正式 [信号回归](../../../tests/common/signal_context_tests.cpp) 将此反例改为必须拒绝的断言，同时覆盖正常 ESR、短/超长/零 size、未知 record 和真实读写异常。该项已关闭。

### 修复后验证

独立构建入口：[tests/common](../../../tests/common/CMakeLists.txt)，同时由根 tests 接入。Android POSIX target 显式取消 `_GNU_SOURCE`，不依赖默认 GNU 配置掩盖问题。

| 环境 / target | 结果 |
|---|---|
| AYN Thor / API33 / ARM64 / 4KiB，`common_error_tests` | 7/7 |
| 同设备，`common_error_posix_tests` | 7/7 |
| 同设备，`common_signal_context_tests` | 50/50，含错误对齐拒绝、20 次真实读 fault 和 20 次真实写 fault |
| macOS / AppleClang17，`common_error_tests` | CTest 1/1，内部 7/7 |

[fixed-manifest.json](2026-09-12-common-review/fixed-manifest.json) 记录修复后源码 SHA、构建命令、本机/设备匹配的二进制 SHA 和退出码；`fixed-*.txt` 保存原始输出。旧 manifest、探针和失败日志不覆盖。仍是 CLI 辅助验证，未扩大为普通 APK、完整信号链或游戏验收。

## 4. 下一次执行必须推进的主线

本轮没有出现新的整体架构分岔，也不需要再次询问是否开始CMake。用户已经授权完整host原生化和默认Turnip。按现有PKG v2连续推进：

1. **立即做正式host构建入口与第一次链接。** 从根CMake源列表提取复用target，串COMMON、实际HLE、CPU adapter、renderer、audio/media/input/依赖；小批次驱动编译及链接错误直到真实 `--no-undefined` `.so`。GNU/API、生成头、libressl/include等由实际target固化。静态archive创建成功不是full-link成功；库未被实际入口引用而被丢弃也不能算闭包验收。
2. **同一版本接通真正的bionic Turnip加载和生产执行。** 默认解析受控的Turnip包，经实际loader得到 `vkGetInstanceProcAddr`，记录库身份和feature bits，不以环境字典代替加载；缺包/不兼容不回退系统。继续真实guest VM、loader/module init、typed HLE、线程/TLS/回调和CPU/GPU tracking，与ANativeWindow/Stop/输入/音频一起组合。
3. **交付真实PKG真机结果。** 从普通UI安装TMNT匹配本体＋更新，实际进入可操作游戏场景、持续十分钟、Stop并同进程重启三轮；AYN与Swan分别记环境和结果。CPU循环、对象编译、first HLE或单帧只作中途定位，不作为完成终点。

原[完整PKG复核](full-pkg-review-2026-09-12.md)和[Vulkan复核](vulkan-review-2026-09-12.md)中的未完成生产边界仍有效，但已修项不重新列为原样缺陷。当前结论是**构建准备有实质进展，整合主线尚未进入全链接/游戏阶段**。
