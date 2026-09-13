# shadPS4 Android / FEX development context

@AGENTS.md

`AGENTS.md` 是共享事实与工程约束；本文件只提供 Claude/Opus 入口。

## 当前交接（2026-09-13）

先读[本轮运行时复核与修复](docs/validation/android-native-host/wp1-mechanism-review-2026-09-13.md)，然后连续执行[TMNT整版spec](docs/specs/android-native-host-tmnt-after-runtime.md)。用户明确要求生产 Linker、VM、线程/TLS、可取消HLE回调由当前复核轮完成，**这部分已经实现，不再交给下一位从头接线**。

生产 `GuestRuntime` 通过 `FexSessionBackend`、JNI 和普通 Service 执行 ELF/SELF；Module/Linker/VM/backing、guest pthread/TLS/errno/stack guard、HleScope/两层InvokeGuest/WaitingHle取消都已组成运行时。AYN/API33/4KiB：guest242/0、contract46/46、ABI14/0、registry13/0、veneer19/0；普通APK12个同PID generation覆盖双模块DT_INIT/TLS、子线程运行期间VM、正常/取消/坏指针/未知import/初始化取消及恢复。host dlopen+85/0、macOS现代LLVM contract45+1SKIP/46、Session807/0。原始证据保留07ce52ae+dirty身份；不能把它们说成TMNT已可玩或Swan已验。

下一阶段扩真实游戏的Orbis函数族和guest libc启动，集成Turnip/Surface/AAudio/FEX-origin pad/内容版本事务，最后UI本体+更新、交互场景、十分钟、Stop、同进程三轮游戏。连续推进两个工作包，不在每个NID/库/首帧处重新交回微型规划。

## 必须保留

- 目标Swan/Android16/API36/ARM64/**4KiB**；AYN为辅助，Swan不在线标NOT_RUN；16KiB和PSVR后置。FEX只跑PS4 x86 guest，host是NDK/bionic原生ARM64。
- host DSO唯一提供GuestAddressSpace和Foundation InputHub/OrbisPadAdapter；JNI/FEX导入生成SDK。无需重做单库。Android已无SDL和冲突JNI_OnLoad，桌面SDL保留。
- Foundation owned `codex/shadps4-android-fex-v0` / `5388ef45313d6c32cb5f4bb5b07f1246ee381370` 已push；FEX维持 `385a0cc4`，不绕过子仓指令改动。依赖修改先查owned ref和[归属](docs/subrepository-ownership.md)，子仓先push再pin。
- FEX使用实际rpmalloc+普通mmap/munmap hooks；不steal ART高VA、不替换bionic malloc、不用Windows HookPtrs。guest owns4MiB–256MiB、4GiB–120GiB，ART hole不在账本。exact reserve碰撞可回收失败，不扫描maps后MAP_FIXED。
- 保留主仓EntryBackedgePass：pinned FEX局部条件回边会跳过entry interrupt poll，G47以真实热循环复现。pass只改guest EntryPoint边；不改REP/原子内部循环、不用MAXINST=1掩盖。
- NON-spill退出JIT后执行HLE，scope控制回调；native fenv/errno、outer continuation、fault/cancel/exit归属保留。VM内部park必须真实退出JIT、释放资格；不能清用户Cancel或带pin跨任意callback。
- SELF原ELF节表offset不是容器offset；模块依赖支持modules/与sce_module/。guard是显式guest数据；未知对象拒绝，未知函数具名fault，不写native函数或global地址到guest GOT。
- 通用guest指令异常、非默认pthread/动态TLS/API覆盖仍有实际边界；新调用沿已接好的runtime完善，不把合成check数当全部HLE语义已经完成。
- 默认Android/bionic Turnip，无静默系统driver fallback；实际loader handle/shaderInt64/namespace/Surface寿命必须验。FFmpeg是独立owned子仓，不塞Foundation。
- TMNT01.08是更新；[本体/更新身份](docs/validation/android-native-host/2026-09-12-review/pkg-set.json)。本轮选择了base后overlay的eboot/模块/param.sfo，不含完整游戏assets。不能拿旧01.00 eboot-only冒充完整1.08启动。
- 不提交游戏/PKG/driver/APK/DSO或凭据；保留无关`references/Bachata-S4`和`externals/dear_imgui/`本地工作。所有失败和旧NOT_RUN保留，不能倒写历史Build ID或源码身份。

## 复现与历史资料

[本轮命令与证据](docs/validation/android-native-host/2026-09-13-wp1-review/README.md)；构建入口`scripts/android/build-host-android`、匹配profile FEX、Gradle的`fexBuildDir`/`hostLoaderConfig`；验证入口`scripts/android/validate-production-runtime-android`。源host与APK剥离符号后的Build ID必须相同。

[输入接通复核](docs/validation/android-native-host/runtime-input-review-2026-09-13.md)保留Foundation49/0+Android5、pad45/0、APK input6等证据。旧Stage0“crt到首个HLE已过”已撤回；prologue harness仅LOAD_AUDIT，不重新用作执行门槛。

[完整资料索引](docs/README.md)、[基础版本](docs/baselines/2026-09-07-android-fex-foundation.md)、[V0](docs/specs/android-fex-v0.md)、[Foundation](docs/foundation-integration.md)、[host→guest LLDB](docs/fex-lldb-host-guest-workflow.md)。历史allocator BLOCKED/无HleScope/未接runtime等描述是旧状态，当前实现以本轮复核和AGENTS为准。
