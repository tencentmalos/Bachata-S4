# 二周目实施进度（2026-09-08）

分支 `codex/android-fex-round2`，起点 `5687da3548f044486c05e997e704fadf44b20e66`（含二周目 spec 的提交）。

**状态：G0 完成并已验证；G1 已实现但未在设备上验证；G2–G4 未开始。距离 ROUND2_ACCEPTED 还有大部分工作。**

依赖未改动：FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`，Foundation `1f7008848736b7c6c0779220480344fd5b5fc5e3`。本轮**没有**修改任何子仓。

## 已完成

### G0 证据与构建基线 — 已验证

| 提交 | 内容 |
|---|---|
| `a5e91d51` | B02 改由 artifact verifier 判定；runner 8 类负例回归 |

- **B02 口径修正**。[verify-native-artifacts](../../scripts/android/verify-native-artifacts) 区分 `coverage: auxiliary`（独立 ELF）与 `coverage: package`（APK 全库 + 依赖闭包 + ZIP 布局）。无 APK 时 B02 记 `NOT_RUN` + `auxiliary_status=PASS`。验收数从 11 PASS 变为 **10 PASS**——第 11 项本来就不成立。
- **对齐真的会拒绝**了，不再只是打印：把阈值提到 `0x10000` 时 ELF 被判 FAIL。
- **R2-B02 的 8 类 runner 负例**：[test-runner-accounting](../../scripts/android/test-runner-accounting)，12/12。
- 这里发现一个真实缺陷：子项解析用 dict 赋值，同一 ID 出现两次时**后一行覆盖前一行**，于是"第 3 次 FAIL、第 4 次 PASS"报 PASS。现在按最坏判定收敛，并记录 iteration 计数与冲突。对修复前的 runner 跑这三项会 FAIL（`status=PASS`），修复后通过。

证据：[g0/](g0/)。

### G1 运行控制 — 已实现，**未在设备验证**

| 提交 | 内容 |
|---|---|
| `898e455a` | FEX 异步停止路径的 pinned 源码证明 |
| `39fb54a1` | RequestInterrupt / WaitStopped / Resume / ContextId 与 spin_loop fixture |
| `c0af37fa` | 中断信号按 bionic 运行期范围解析 |

先做了 spec §3.1 要求的[源码证明](../../fex-async-stop-source-proof.md)，两个结论改变了实现方式：

1. spill 入口二选一由 `IsAddressInCodeBuffer` 决定，选错会让 CPUState 及其快照全部陈旧。
2. **FEXCore 的 `SleepThread` 默认是空函数体**，本仓未覆写。也就是说改之前发 pause 信号会 spill、立刻返回、恢复——看起来接通了，实际什么都没发生。等待逻辑必须由本仓在该覆写点实现。

好消息是全部走公开 API，**G1 不需要改 FEX 子仓**。

实现要点：handler 内不分配、不加锁、只做原子操作和两次 ucontext 写，并链接到 previous disposition（ART 有自己的 handler）；ack 只在 spill 完成后由 owner 自己发布，所以 `request_epoch` 与 `stop_epoch` 是两个数；`Resume` 只消费指定 epoch，`Run` 在有更新请求未处理时拒绝进入 JIT；ticket 带单调 context id，避免旧 ticket 命中新 context；`deadline_ns` 由静默忽略改为执行前 `Unsupported`。

新增 `spin_loop` fixture（`xor/inc/jmp`，无出口），从源码汇编并反汇编回来。这是协作式停止无法处理的用例。

## 未完成

**G1 未验证。** 设备在本轮工作中途从 USB 断开，之后一直不在线。G11/G12 能为 arm64 编译链接，但**没有执行过**。信号号 `SIGRTMAX-1` 是基于 ART 用法的推理选择，不是实测结论。恢复设备后需要跑：

```sh
cmake --build build/v0-fex
adb push build/v0-fex/guest_execution_tests /data/local/tmp/g && adb shell chmod 755 /data/local/tmp/g
adb shell /data/local/tmp/g
```

**G2、G3、G4 未开始。** 三者都依赖真实执行，在设备恢复前无法推进：

- G2 运行中事务：context quiesce、token remap、双 owner 100 epoch 发布。
- G3 真实 typed HLE：guest gate、长度/pin、TLS、两层 callback。
- G4 普通 APK：Activity/JNI、Foundation 闭包、ART 共存、LLDB 安全点、soak。

24 项 R2 验收目前 **0 项达成**：R2-B02 的 runner 负例虽已通过，但该项完整语义还要求 app 环境下的重复记录判定，须在 G4 一并验证。

## 复现

见 [g0/README.md](g0/README.md)。host 回归保持 contract 33/34 + 1 SKIP、HLE 14/14、page probe 全通过，与一周目一致。
