# WP1 复核与原生 Turnip／Session 图形接入（2026-09-13）

本轮在主仓 `codex/android-fex-round2` 的 `10a5b23e` 上直接修复并继续实现，没有新增执行 spec。FEX 子仓仍为 `385a0cc4`，Foundation 为 `5388ef45`，均未修改。

## 复核结论

上一轮确实执行了真实 TMNT 的 libc bootstrap 和六个依赖 DT_INIT，但“整个非图形启动完成”超出了证据范围。到达第一个 VideoOut import 只能证明此前走过的路径，不能证明游戏后续不会调用其他非图形 API。

发现并直接修复：

- `DT_NEEDED` 中出现系统库名字不等于 provider 已准备好。`GuestSysmodules` 只接收成功初始化的 guest module 或明确实现的 host provider；维护稳定 handle、受限 refcount 和每代状态。TMNT 请求缺失的 `libSceSaveDataDialog` 时现在返回 `0x805a10ff`，游戏容忍该失败并继续。
- User/System Service 原先仍调用桌面全局队列及用户管理状态。现在使用会话持有的用户快照、初始化状态和事件队列，输出通过 guest 地址空间检查后复制，不泄漏 native 地址。
- Bind 的 libkernel 入口原可绕过其他库的 NID allow-set；已限定库与函数族。GnmRegisterOwner 调用真实零售失败实现时传入空 native 指针，不再传 guest 指针。
- 图形初始化暴露 PageManager 懒建 `Core::Signals`，覆盖 FEX 的 SIGBUS／SIGSEGV 等处理器。真实 APK 曾因此在 FEX JIT 中进入桌面 `Core::SignalHandler` 并断言。现在 Graphics 持有 passive SignalDispatch，构造、RemoveHandlers、析构均不改变 OS signal actions。Android 断言直接保留 debuggerd 调用栈，不先重置 FEX／ART handlers。
- 完整旧用例重新暴露 VM 发布与继续执行之间的竞态：观察 epoch A 结束后，epoch B 可在 AcquireExecutionLease 前开始。主仓 FEX adapter 在同一有界期限内重试准入，取得 lease 与发布 running 共享 coordinator lock，不重跑 HLE。线程创建／销毁与生产 VM mutex 协调，防止正常 pthread exit 变成 Busy。
- 生产内部暂停不再触发持久 HLE cancellation source；已离开 JIT 的 HLE 边界可由 execution lease／pin 规则参与 drain。外部 Cancel／Shutdown 保持独立优先级。G48 强制将第二次发布放进上述窗口，实际观察 Busy 分支，验证 33 次恢复及 token 仍持有时取消。

## 已实现的图形路径

普通 APK 打包独立 bionic Turnip 与全部四个 adrenotools hook DSO。Hook 只作为加载资源打包，不能成为 JNI DSO 的 DT_NEEDED：提前自动加载会让未初始化 hook 截获 libc 调用，本轮已实测并修复。

固定包记录见 [turnip-bionic.json](../../../runtime/locks/turnip-bionic.json)，下载／校验入口为 [prepare-bionic-turnip](../../../scripts/android/prepare-bionic-turnip)。来源是 [K11MCH1 的 v26.0.0-rc08 release](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v26.0.0-rc08)。不提交预编译驱动，不复用原来的 glibc EMULATOR ZIP。Kotlin 与 native 均检查 ELF SHA；没有系统驱动 fallback。

实测驱动：Turnip Adreno 740，`Mesa 26.0.0-devel (git-5ac41be677)`，`shaderInt64=1`。驱动包发布标签、实际 runtime driverInfo、ELF Build ID 分别记录，不能相互替代。adrenotools 没有 namespace teardown API，因此每进程只创建一次固定 loader，窗口／Vulkan Instance／Presenter 随 Session generation 重建。

SessionParams 由 SessionCore 写入 generation，生产 backend 创建 AndroidWindow 并装入 verified driver。Service 等待有效 Surface，接好 NativePadBridge 后回执 PlatformReady，guest 才可 Run；Surface 丢失或更换会停止旧代。Graphics 在首次 VideoOutOpen 创建 Liverpool、Presenter、私有 VideoOutDriver 和 IRQ binding。Stop 先请求各 worker 退出，join 完后再销毁 port、renderer、window 与 guest VM。

已准入：VideoOutOpen、GetResolutionStatus、SetBufferAttribute、GetVblankStatus。缓冲属性用 CallCursor 解第七个栈参数，native 只操作本地结构，guest 输出保持 writable pin；未知格式具名失败，不进入桌面的 assert。

## 验证与边界

证据根目录：[2026-09-13-wp1-review](2026-09-13-wp1-review/README.md)。运行设备为 AYN Thor / API33 / ARM64 / 4096-byte pages；ordinary APK UID 不是 shell。Swan/API36 未验，16 KiB 不在本轮范围。

最终结果：

| 验证 | 结果与证据 |
|---|---|
| 完整 host DSO / NDK API33 | HOST_LINK_PASS，`--no-undefined`；[构建身份](2026-09-13-wp1-review/wp2-build-delivery/archive.json) |
| host DSO CLI 合约 | **198/0**，包含三轮 passive dispatcher／八种信号不变；[manifest](2026-09-13-wp1-review/wp2-host-delivery/result.json) |
| FEX 实际 guest / CPU contract / ABI / registry / veneer | **244/0、46/46、14/14、13/0、19/0**；[manifest](2026-09-13-wp1-review/wp2-cpu-delivery/manifest.json) |
| 平台服务 / SessionCore | 设备 **70/0、840/0**；现代 macOS 同样 **70/0、840/0** |
| 原生产链回归 | **10组CLI × 3轮**；普通 Service APK **16代同PID**；[manifest](2026-09-13-wp1-review/wp2-runtime-delivery/manifest.json) |
| Turnip／WSI | 普通APK **2个JUnit测试**，含真实三轮Surface/swapchain重建 |
| 真实 guest 图形正负例 | **1个JUnit测试、5代同PID**：正常/坏地址/恢复/坏格式/恢复；检查七参pitch、渲染后未对齐原子、实际vblank≥3 |
| 真实TMNT边界 | **1个JUnit测试、3代同PID**，均 graphics=ready 后精确停在 RegisterBuffers；[图形与内容manifest](2026-09-13-wp1-review/wp2-graphics-final/manifest.json) |

最终 APK 中 host Build ID 为 `9c8df3d6af500a24994f3c3f388ca1b37843eb07`，JNI/FEX 为 `b903766b1155f68af99c7c6369156072faeafda4`。产物是在 `10a5b23e` 加记录的源改动上构建；提交不会改写旧 SCM 身份。host 无 SDL 定义，生产 runner 无新增测试 gate 定义。测试临时 shell 目录和本轮 app-private 选取内容均已清理，保留已安装 APK／驱动。

复现入口：`scripts/android/build-host-android`、`scripts/android/validate-production-runtime-android`；额外 APK JUnit selectors 为 `TurnipInstrumentedTest`、`RenderedRuntimeInstrumentedTest#syntheticVideoOutValidatesStackArgumentsFaultsAndRestarts`。真实内容测试必须显式提供 `contentRelativePath`，否则为 NOT_RUN，不能用无内容的测试跳过充当通过。构建传入 matching `-PfexBuildDir` 与 `-PhostLoaderConfig`；详见各 manifest 的命令、路径和 hash。

最终验收与源／产物 hash 以随本报告交付的 `wp2-*-delivery`、`wp2-graphics-final` manifest 为准。原始失败保留在 `wp2-runtime-final`、`wp2-runtime-join-diagnostic`、`wp2-runtime-continuation`、`wp2-runtime-retirement`，分别记录执行准入 Busy、线程销毁 Busy 和诊断过程，不改写为成功。

真实 TMNT 使用已核对的 base+update **选取 executable/modules/param.sfo**，没有完整 assets。当前达到实际 native Turnip／Presenter／VideoOut 初始化，然后在 `sceVideoOutRegisterBuffers`（`w3BY+tAEiQY#libSceVideoOut`，op153）具名 Faulted。不能把这称为游戏帧、可玩场景或 WP2 完成。

后续实际缺口集中在 GPU buffer/submit/flip：guest 范围与尺寸验证、registration 事务、buffer/label 寿命、GPU tracking 与 guest VM protection reason／FEX fault delivery 的组合、event/IRQ guest 语义，以及 driver wait/error 的完整 Stop 行为。当前 passive dispatcher **没有**宣称接通 GPU fault recovery；不应仅扩 allow-set 或直接套用桌面 raw pointer 接口。

完整 assets/UI base+update 事务、FEX-origin pad、AAudio 游戏输出、十分钟交互及三轮游戏重启仍待验证。上述已实现路径不应再交回另一份“初始窗口／Turnip／CPU runtime 尚未接入”的规划。

## 用户补充：存档是本版必需能力

`libSceSaveDataDialog`（sysmodule id `0xa0`）负责存档交互，`libSceSaveData` 负责存储。前文返回 `0x805a10ff` 仅表示尚无 provider；游戏当前容忍该返回值，不能据此把存档从本版目标移除。

代码复核：桌面 [savedata.cpp](../../../src/core/libraries/save_data/savedata.cpp) 使用全局初始化状态、游戏标识及 mount slots；[savedatadialog.cpp](../../../src/core/libraries/save_data/dialog/savedatadialog.cpp) 使用全局 dialog 状态及 UI。二者目前均不在生产 GuestRuntime 的准入集合中；编进 host DSO 不等于 guest 已能安全调用。

在当前整版实施中复用已有存储后端，接通会话持有的状态、guest 嵌套结构与指针校验、文件系统挂载、初始化/卸载/Stop 回收。持久数据按稳定的用户与标题标识隔离，不能跟随 generation 或临时内容目录删除；本体更新不应更换或覆盖存档根目录。Dialog 的状态、选择、取消及结果必须来自真实交互或实际操作完成，不能用无 UI 的假成功代替。

验收必须包含真实 guest 创建/写入/卸载存档、正常退出及进程重启后读回一致内容、本体更新后仍可读取，以及坏指针、只读挂载、I/O 失败和取消。当前这些生产存档验收均未完成；此补充只明确用户要求和代码现状，没有宣称新实现或新测试通过。
