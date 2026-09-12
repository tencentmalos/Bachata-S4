# G2 剩余工作执行 spec：关键修复后的下一阶段

**后续入口：[e24aad69 收口复核](../validation/round2/g2-close-review-2026-09-09.md) → [G2 出口/G3 入口 spec](android-fex-round2-g2-exit-g3-entry.md)。以下保留上一阶段要求和当时状态。**

日期 2026-09-09。先读 [本轮修复记录](../validation/round2/g2-repair-2026-09-09.md)、[progress](../validation/round2/progress.md) 和 [Round 2 主 spec](android-fex-round2.md)。本文件是下一执行入口；[G2 repair spec](android-fex-round2-g2-repair.md)保留完整修复要求，但不要把已经修好的 P1 重新当作全未实施。

当前基点为 `5304e5a9` **加本轮未提交修改**，准确源码身份见修复 manifest。先核对 hash/status，保存有意修改；不得 reset 到旧 HEAD 而丢失修复。FEX/Foundation pin 保持不变。目标 Swan / Android 16 / ARM64 / 4 KiB；Pocket DS API 33 仅作 CLI 辅助，16 KiB 不重新列为阻塞。

## 交接状态

| 规范项 | 当前能证明 | 尚缺 |
|---|---|---|
| R2-M01 | 单次停止双 owner；串行 fresh-thread 100 轮发布 smoke | 同一双 owner/handle 跨版本执行 100 epochs |
| R2-M02 | 早关闭 gate；两种 Cancel 时序；一次 hold 超时→重试；lease/pin drain contract | 100 轮 late ack/外部 Pause/Cancel/Shutdown；sink/drain/生命周期完整交错；WaitingHle |
| R2-M03 | 同一热线程 RW Clear 后实际执行 B；RX Clear 再 Invalidate；remap 先失效旧译码 | 双 owner 持续 remap 100 epochs；完整状态保持；多映射/removed-range/权限查询矩阵；系统调用失败注入 |
| R2-M04 | 未实施 | 真实 guest store 改另一段热代码→host 协调发布，至少 10 次；真实 HLE 合并 G3 |
| R2-M05 | 旧 poison/lease/sink 回归和新 mutator 排他通过 | 新入口全部失败路径、request/fault/生命周期矩阵、24 项结果与 APK 环境闭环 |

已完成机制不得回退：interrupt-page JIT entry stop、owner Run 返回、process-lifetime FEX allocator；BeginDrain/FinishDrain；只消费内部 Pause；shared+thread-local invalidation 锁；sink 区间禁止 mutator 和新 pin；失败 poison。禁止 SleepThread parking、host SIGILL 恢复和 settle sleep。

## N1：真正的持久双 owner 验收（优先）

对应 R2-M01/M03；参考旧 G21/G22 的发布流程，但不能继续使用它们的 fresh-thread helper 作为验收。

1. 汇编 fixture 同时输出独立 version/progress。两个 native owner 各 CreateThread 一次，100 epochs 内真实 TID、handle/generation 保持不变，最后各自在 owner 上 DestroyThread 并检查结果。
2. 每轮先观测两者都执行旧版本并增加 progress，再 QuiesceContext，验证两个 owner Run 返回；token 内 RW、发布/失效、最终 RX，释放后同一两个 owner 再运行，均只输出新版本并继续推进。
3. 分别执行 publish 100 epochs 与 same-VA remap 100 epochs。保存逐 epoch 记录，包含 TID、handle/generation、old/new version、counter、stop/request/ack epoch、mapping/code generation 和耗时。不能仅输出总 PASS。
4. Remap 与 Clear 必须分别证明。Remap 已自动调用 sink，Publish 也会失效，因此 **不得用 Remap+Publish+Clear 后的新结果宣称 Clear 生效**。保留 G23 的独立路线；扩展多映射与完整 GPR/XMM/MXCSR/FS/GS、guest memory 保持，排除测试有意修改的版本区域。
5. 所有 waits 用可观察进度/barrier 和绝对 deadline；退化为只创建一个 owner、其中一个不更新或循环提前退出均必须 FAIL。不要用固定 sleep。

完成后仍属 CLI auxiliary；同样用例需被 G4 的普通 APK 承载才满足目标环境。

## N2：协调、权限及失败矩阵

对应 R2-M02/M03/M05。扩展 G24/M24–M26，保留已有修复后回归。

- 将协调场景扩展为 100 轮：Pause/Cancel/Shutdown 在内部 request 前/后、部分停止、迟到 ack、fault 优先、并发 coordinator、Run 在 admission 边界已持 lease、Create/Destroy/generation、owner 自等待。按实际支持的状态拒绝，不假造 WaitingHle。
- token、未完成 drain、context/space 的销毁/重建与迟到释放必须有明确契约。当前超时恢复是 retry→完成排空→释放 token；如增加 Abort，只有确认安全且不丢外部请求才能重开。禁止直接清 active epoch 解锁。
- 用 barrier sink 分别阻塞 Publish 和 Remap；同 token 的 Reprotect/Remap/Publish/Invalidate，以及 read/write pin 均须 Busy。加入注销/draining、throw、多个独立 poison range、恢复失败再恢复成功。
- 注入真实 mmap/mprotect 失败（受控系统调用封装或链接测试 seam，生产默认必须调用真实 syscall）。断言元数据、旧/新 backing、权限、执行准入和错误身份；不要把 invalid token 当系统调用失败。当前 mmap 失败已保守 poison，但尚无注入证据，不承诺 rollback。
- 专测 QueryGuestExecutableRange：多映射、曾经 RX 后 RW、Unmap/同 VA remap、移除后 Query、decoder range cache。检查 permission-only Reprotect 与 decoder/旧译码的交互：若需要 caller 显式失效，契约和拒绝执行的门禁必须保证遗漏失效不能运行旧代码，不能只让 Query 返回新权限。
- 完成 checked range 的 zero/overflow/out-of-reservation、foreign/default/moved/stale token、pins 重入和 mapping generation 测试。

N1/N2 任何实际错误先修再推进，不以 suite 总数掩盖。每种失败均须证明 token 释放/Resume/新 Run/另一个事务不能绕过，且有受控恢复路径。

## N3：真实 guest store 发布子路径

对应 R2-M04：准备两个独立代码区，一段已被两个 owner 热执行为 A，另一 guest fixture 实际 store 将其改为 B；由 host coordinator 完成 ExplicitPublication，再由原线程执行并断言 B，至少 10 次。不得用 host memcpy 代替 guest store。记录修改前后的实际字节、版本、progress 和 generation。

TransparentSmc 继续 Unsupported。尚无真实 HLE 时，完整 R2-M04 保持待完成；host-coordinator 子路径通过是辅助结果，不能冒充 guest→HLE gate 的验收。

## N4：证据、runner 和可交付基点

1. 先提交明确的源代码/测试/文档分组；本轮未提交的修复必须包含在交接中。`tests/runner/test-g1-evidence.py` 已核对历史身份并为混合 host/guest 映射更新，应明确纳入版本控制。
2. 继续使用 canonical `cmake/fex`；fresh embedder build 复用 FEX 静态库时如实记录，不称全依赖 clean build。锁定源码/fixtures/runner/静态库 hash、binary Build ID、设备 SHA。
3. 新 case 接入 ownership 和缺项/失败/超时/重复 ID/SKIP 回归。输出 24 项 R2 与完整 V0 两种视图，区分 auxiliary/目标环境。当前 V0 10/0/47 只是当前结果，不手工累加新的 PASS。
4. 保留中途失败和 timeout，归档每轮日志，更新 progress/AGENTS/CLAUDE。提交前检查主仓与所有相关子仓；Windows/Vortek 文档、Bachata-S4 本地改动、externals 临时目录不混入本批。
5. 若需改变子仓，先读 ownership 和该子仓规则并用 gh 核对 tencentmalos 的 owned repo/ref；当前修复不需要 FEX 子仓变化。推送 child 后才能更新 parent gitlink。

## 然后进入 G3/G4

G3 先做每线程 syscall/HLE 错误归属（当前 unexpected-syscall 标记仍为 context 级）、typed HLE gate、host/guest FP 切换、长度/方向 pin、返回值/errno/TLS，再做两层 InvokeGuest 和可取消 WaitingHle。G2 的真实 HLE 交叉项在此补证，不能提前关闭。

G4 再接 ordinary APK/JNI/ART、Foundation lifecycle、LLDB 只读 guest snapshot，并补 package verifier ABI/closure/ZIP 闭包检查。Swan Android 16 / 4 KiB 的 app 环境是正式验收目标。finite Step、Vulkan presentation、游戏和 PSVR 仍是后续 V0/产品阶段，不扩入这份 G2 收尾任务。
