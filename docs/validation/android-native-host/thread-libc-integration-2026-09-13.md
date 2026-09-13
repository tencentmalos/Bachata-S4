# 桌面 libc 策略与 guest 线程族直接补齐（2026-09-13）

本轮从 `f4ba0623783136c010492c4142eb0da0288ee2c5` 的 `pthread_attr_init` 边界继续直接实现；没有新增执行 spec。用户要求每个缺口先对照桌面版，按函数族及其实际运行时依赖一起处理；只做改动相关测试，不做完整回归。

## 为什么 libc 看起来一直缺

[桌面 Linker](../../../src/core/linker.cpp) 的 `Execute` 优先加载配置目录中的 `libSceLibcInternal.sprx`，没有文件才注册 [LibcInternal HLE](../../../src/core/libraries/libc_internal)。该组 HLE 有 58 个函数注册；桌面可直接调用本机 x86 代码。Android production `GuestRuntime` 独立于这条 native Execute 路径，原先既没有完整系统 libc 的优先装载，也仅对少数 stream/tag 做兼容映射，所以真实游戏每遇到一个 Internal 导入就再次暴露缺口。

不能把桌面 native 函数指针直接填入 ARM64/FEX guest GOT，也不能把 x86 `va_list`、`FILE`、allocator/mspace 或析构回调交给 bionic 同名函数。当前兼容策略复用真实游戏 `libc.prx` 的 x86 导出，保留 guest ABI 和对象所有权。

## 已实现的整组处理

- **完整系统 libc 优先。** `GuestRuntime::Prepare` 读取 `EmulatorSettings.GetSysModulesDir()/libSceLibcInternal.sprx`，作为 guest 根模块进入相同的依赖、TLS、重定位和初始化图。Android 默认目录为 app 私有 `files/host/sys_modules/`。保留 `_malloc_init`、可选 mutex-enable 与 DT_INIT 顺序。重复 provider、错误库标识、装载/初始化失败会失败；一旦选中系统 provider，不再用游戏 libc 混补其函数、FILE 或依赖标记。
- **显式兼容族。** 新增 [guest_libc_policy.h](../../../src/core/host_runtime/guest_libc_policy.h)，覆盖 desktop 注册以及实际 stdio/printf、math、memory/string、时间、C++ runtime 与 mspace 入口，共 112 个精确 NID。仅当没有完整系统 provider，且库/模块/版本/函数类型全部匹配时，查找唯一同 NID 的 guest `libc#1#libc` 导出；还检查定义 ELF 的可执行段与所属 guest VA。重定位时页仍 RW，不能错误地要求最终 Execute 页权限。
- **线程属性及实际创建。** 会话拥有 attr handle，支持 stack/size/guard、detach、inherit/policy/priority/scope、affinity、getattr；`pthread_create` 真正消费属性副本并握手确认 native startup。调用者提供的栈只使用其指定范围，绝不保护相邻页；有独立 TLS，attr 销毁不改变已创建线程。补 equal/thread-id/yield/live scheduling 及完整 u32 usleep 和 sleep 别名。优先级是桌面相同的模拟器元数据，不是 Android 实时调度保证；affinity 实际映射到可用 host CPUs。suspend/resume 尚无实现，明确返回 ENOTSUP。
- **mutex/rwlock 族。** 增加 guest-owned rwlock 的属性、init/destroy、读写/try/timed/unlock，支持多个 reader、owner 检查、可取消等待、销毁 Busy。mutex protocol/ceiling/type/pshared 按 desktop 实现保存并在创建时复制到真实锁与 guest prefix；修正 legacy kind ABI 和 POSIX/Orbis 错误码包装。desktop 对非 None protocol 跳过 adaptive spinning，仍使用普通阻塞锁，未实现优先级捐赠；Android 保持这一模拟语义，并不宣称具备实时继承/ceiling 调度。
- **两处并发根因。** FEX syscall 退出进入 HLE 时补发停止状态通知，并恢复已经被 Pause/Cancel 保护的 poll page，但不消费中断 ticket，避免 VM drainer 等满超时及返回后陈旧 page fault。mutex/rwlock 短写回和 thread-attr 操作通过生产 VM mutex 与发布串行；不持 pin/VM mutex 跨 mutex/cond/rwlock 等待，也不解除 address-space 的 Busy 保护。后台 guest fault 会保留原 thread/context/generation/import 并取消其他 owner，防止主线程永远等已失败的子线程。

## 验证口径

辅助设备 AYN Thor / API33 / ARM64 / 4 KiB，普通 APK UID10157；不是 Swan/API36 验收。只运行改动相关目标。

| 检查 | 结果及范围 |
|---|---|
| NDK host DSO | `HOST_LINK_PASS`，完整 `--no-undefined` 链接 |
| thread-attribute domain | 43/0，坏地址/范围/副本/销毁与属性矩阵 |
| mutex/rwlock/libc policy domain | 60/0，包含 VM 发布期间的 mutex 写回排他反例 |
| FEX focused publication | G49a–d + G48a–b，6/0；真实 syscall/coordinator 确定交错、Cancel 与 33 次 continuation admission 竞争 |
| 普通 APK thread test | 属性创建、默认/调用者栈实际 RSP、独立 owner、protocol mutex 与 rwlock、Orbis Busy；另一个 fixture 验证子线程具名 fault |
| 普通 APK libc policy | fallback → system 优先 → `_malloc_init` 失败且不回退 → 删除后恢复 → 旧 bootstrap 正常；五种模式 |
| 真实 TMNT | 同一 PID 三轮均到 `sceSysmoduleLoadModuleInternalWithArg`（`hHrGoGoNf+s`, op527, category10/Unsupported），guest_presents=0；仅边界观测通过，游戏仍未运行到画面 |

整库选择测试使用自制 x86 provider（特意返回不同标记来证明选择路径）。本机未找到可用于本轮验证的完整系统 `.sprx`；没有声称真实固件 libc 已验收。[静态核对](2026-09-13-thread-libc/libc-audit.json)中，112 个策略入口有 102 个真实游戏 libc 导出；其余 10 个私有 helper 仍会具名拒绝。此前被拒绝的当前 TMNT 27 个 Internal 函数导入均可解析；这是导入可用性，**不是 112 个函数全部执行通过，也不是整个 libc 已无 kernel 依赖缺口**。

实际内容仍只有本体+更新选取的 eboot/模块/param.sfo，未铺完整 assets，没有完成 UI 安装、交互场景、十分钟游戏或 Swan 验收。

## 保留的失败与修正证据

[证据目录](2026-09-13-thread-libc/README.md)保留失败，不倒写旧 source identity：

- APK first：fixture 误将退出码放 EDI；修成 EAX 后通过。
- APK third：两轮 vprintf 具名边界、一轮超时；随后补子线程 fault 传播。该超时不单凭推断宣称已定位为同一原因。
- APK fourth/fifth：对 RW 重定位页误查 Execute，Prepare PermissionDenied；改为检查 ELF 执行段。
- G49 before：syscall 边界漏唤醒，等待 1500ms；只补通知后仍有陈旧 poll page 问题。补 page 恢复后 4/0，再加入 Cancel 形成最终 6/0。
- APK eleventh：protocol 返回 ENOTSUP 后 TMNT 触发 SIGILL；这是失败，不是具名边界通过。按 desktop mutex 实际处理改为复制 protocol 到真实锁；不把 host JIT PC 当精确 guest fault RIP。
- APK twelfth：mutex Write 与 VM 发布竞争，底层明确报 Busy；部分轮先遇到后续 sysmodule 导入。补 VM 串行门闩并加 60 项中的针对性检查。
- rwlock-desktop：误在重建完成前发起测试，跑到旧 41 项 binary，已明确标为 INVALID_EVIDENCE。之后等待构建完成，新 protocol 59/0、含 VM 门闩最终 60/0。

## 后续执行规则

遇到新导入先查看 desktop 的注册、真实实现、模块提供者及调用链，区分已有实现可迁移、依赖实际初始化、桌面本身的 stub/局限；以完整函数族和生命周期补齐，不因一个新 NID 就另写微型 spec。保留 guest/host 隔离和具名失败，不能用任意 NID 别名或返回 0 代替缺失服务。继续现有 TMNT 整体目标，本轮不创建新规划。

本轮最终源码身份仍记为 `f4ba0623+dirty`，不是构建之后的提交号。APK host/JNI 的 Build ID、SHA256 以及本轮文件 SHA 见 [最终 manifest](2026-09-13-thread-libc/apk-thirteenth/manifest.json)；FEX 子仓保持 `385a0cc4`，Foundation 保持 `5388ef45`。未修改 desktop 线程实现或子仓。
