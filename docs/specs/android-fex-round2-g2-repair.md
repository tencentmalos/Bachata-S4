# G2 修复与验收完整要求

**执行入口已更新：[第一轮修复结果](../validation/round2/g2-repair-2026-09-09.md) → [剩余工作 spec](android-fex-round2-g2-next.md)。本文件保留完整要求，不表示所有条目仍未实施。**

日期：2026-09-09。被审基点 `5304e5a9831a4d6ccbaa776b854685332f26d23b`，工作分支 `codex/android-fex-round2`。本任务取代旧 G2 handoff 的执行顺序；规范编号以 [Round 2 主 spec](android-fex-round2.md) 为准。先读 [G2 复核](../validation/round2/g2-review-2026-09-09.md)、[原始证据](../validation/round2/g2-review-2026-09-09/README.md) 和 G1/事务加固记录。

**目标：关闭已复现的 G2 缺陷，再建立真实的持久双 owner 验收，之后才进入 G3。** 不把“85/85 原 suite 通过”作为本任务完成判据。目标仍为 Swan / Android 16 / ARM64 / 4 KiB；可先在 Pocket DS 做 CLI 辅助验证，但正式 app 状态不能提前转 PASS。

## G2R-00：固定基点和证据口径

- 检查主仓与子仓 status；保留无关 Windows/Vortek 文档、Bachata-S4 本地改动和 externals 临时目录。
- `tests/runner/test-g1-evidence.py` 当前未跟踪；对照 G1 manifest 核对后补入明确提交，不得在 fresh checkout 中暗中依赖它。
- 复现原 guest 85/85、Android contract 37/37、macOS 36 PASS + 1 SKIP，并记录源码/fixture/runner hash、binary Build ID、部署 SHA。复用预编 FEX 时明确写明，不宣称全量 clean build。
- 从本次探针提取自动回归。诊断探针的 exit 0 只是输出观察，必须改成契约错误使测试失败；不能照搬它的 exit code 当 PASS。
- 规范映射：M01=持久双 owner 跨版本；M02=协调排他/失败/外部请求；M03=持久 thread remap/cache/range 同步；M04=guest store 发布；M05=所有新入口保持保护。

## G2R-01：重做协调 admission 和请求所有权（R2-M02/M05）

先提交状态机与锁序技术决定，再写实现。至少区分 Idle、Closing/Draining、Quiescent、FailedClosed；函数名可沿用，不能仅靠 `IsQuiescent()` 在最后一个阶段关闭准入。

1. 关闭新 Run/CreateThread/Resume/普通 writer **先于**参与 owner 快照。入口与关闭动作必须线性化；已在入口持有 lease 的操作也必须被纳入 drain。
2. 对参与 owner 先发出全部 stop 请求，再等待；使用同一绝对 deadline，不能给每个线程重新增加一个完整 timeout。并发 coordinator 必须明确 Busy；owner 自等待、销毁/重建 generation、迟到请求都要有定义。
3. 记录属于事务的请求，消费时仅移除本事务已确认的 pause。保留外部 Pause/Cancel/Shutdown、fault、请求 map/receipt 的一致性，禁止 `pending=0` 清空。释放事务不能意外恢复用户本来暂停/取消的线程。
4. 两个阶段分开：Closing/Draining 只是禁止新工作，只有所有参与 owner 确认停止、execution lease 排空、pin/sink 条件满足后，才授予能变更 mapping 的 token。
5. timeout/部分停止/失败不得假装所有 owner 已停止，也不得自动开放新执行。提供明确的有界清理/安全回退方式：在实际排空和处理未完成请求后，才能由明确动作重开。不要留下永久不可回收的匿名 gate。
6. token 与 context/space 生命周期、generation 绑定。token 释放只能结束自己的事务，不能解除更新事务的门禁；沿用已有 liveness 安全性，避免裸 context 指针回调。

回归至少包含：两 owner 中一个被有界 hold、停止过程中第三 owner 创建/Run/Resume/writer、timeout 后新准入、并发 coordinator、100 次外部 Pause/Cancel/Shutdown 竞争、100 次迟到 ack、故障优先级和安全清理。不得用固定 settle sleep 代替应有的同步；测试 hold 的进入/退出需要显式 barrier，所有 future/process 都有超时。

## G2R-02：统一 token mutator 的排他和失败保护（R2-M02/M03/M05）

- Remap/Reprotect 与现有 Publish/Invalidate 共享 token provenance/epoch、sink in-flight/draining、pin、poison、mapping generation 检查。不能通过删普通 Map/Protect/Unmap 的 Busy guard 实现 token 通路。
- 在 Publish 已复制字节而 sink 尚未完成时，另一线程携同一 token 的 remap/reprotect/再次发布必须 Busy，且字节、权限、generation 保持预期。验证 sink drain 和注销并发同样拒绝，不只是一个成功 sink。
- Remap(RX/RWX) 不得直接给 poisoned 或尚未完成失效的新 backing 执行权限。backing 已变更但失效失败/抛异常时必须进入可追踪的失败状态；释放 token、Resume、新 Run、setter、后续独立事务都不能绕过。
- 注入实际 mmap/mprotect/失效失败：断言旧/新 backing、元数据、执行权限和 poison 的明确结果。不要把“默认空 token 被拒绝”命名为“remap 失败注入”。范围运算使用 checked helpers。
- 发布最终 RX 权限与恢复 admission 均应在协调事务的受控提交路径中完成；不在 token 已释放后留下一个普通 Protect 的竞态窗口。

关闭标准：本次 sink-race 的 `publication success + actual byte=0` 不再出现；poison-remap 的 RX 被拒绝；旧 M13h–o/G10 及新负例全部通过，且失败状态可按定义修复。

## G2R-03：真正清理 FEX cache，并同步执行范围（R2-M03）

- 修复 ClearCodeCache 的缺锁调用。复用/抽取已有正确的失效协议，检查 FEX exclusive mutex、共享 code buffers 和每个存活 thread 的 local lookup、decoder range、call-return cache；不要递归拿同一把 context mutex。
- 验证 token 属于本 space、当前协调 generation/epoch、无 running owner；其他 space 的 valid token、过期 token、运行态调用均在触碰 FEX 前拒绝。
- 清理不能只按当前 Execute 权限枚举：已译码后转 RW、remap、被移除 range、后端 return gate 都需定义覆盖方式。使用 pinned FEX 明确支持的全清/范围失效协议，不盲目用巨大的地址范围，也不根据未经验证的“host 地址越界”假设排除范围。
- 失效完成后才返回成功，并保留 guest GPR/FP/内存值。正常失效、重新编译和第二次 Clear 必须在预算内完成。
- 同步 FEX `QueryGuestExecutableRange` 的注册/移除/权限和 decoder cache。目前 append-only 表不能支撑 removed range 验收。权威映射与 FEX 的视图需有一致的 generation，注意查询/回调锁序。

必须具有能单独证明 Clear 有效的测试：先热执行 A；同一存活 guest thread 在 token 下换 B 字节，避免 PublishCode 的额外失效掩盖结果；Clear 后再执行应为 B。RX 路径、RW 路径、多映射路径各验证；Clear 后再 Invalidate/Compile 不得挂起。故障清理和默认/foreign/stale token 拒绝分别记录。

## G2R-04：替换不成立的 G21/G22 验收（R2-M01/M03）

- 创建两个长期存在的 native owner，每个 owner 只 CreateThread 一次；100 epochs 中保持真实 gettid 与 guest handle/generation 不变，最后由各 owner DestroyThread 并检查成功。
- 增加含独立 version 和 progress 输出的汇编循环 fixture。每轮先证明两个 owner 都在执行旧版本，再由 coordinator 关门/停止、变更代码/缓存、提交，之后两者只输出新 version 并继续 progress。
- M01 做持续同 VA publication，M03 做同 VA backing 替换、复用 thread、Clear 后状态保留与新译码；两者分别 100 次。不能用串行 fresh-thread 常量返回合并代表这两项。
- 每轮记录两个 native TID、guest handle、旧/新 version、guest progress、request/ack/stop epoch、mapping/code generation、耗时和退出原因；每次 iteration 必须真正检查结果。
- 覆盖 token 持有期间 Run/CreateThread/Resume/writer 拒绝；关闭时使用精确错误码而非“任意失败都算阻止”。破坏任一 owner 的发布/失效应让整个验收 FAIL。

## G2R-05：补 guest store 子路径和结果归档（R2-M04/M05）

先完成真实 guest store 改另一段已热执行代码，再经 host 协调者 ExplicitPublication 执行新版本，至少 10 次；真实 HLE gate 部分与 G3 合并验证，保持完整 M04 待完成标记。已有 `TransparentSMC` 拒绝继续保留。

扩展 runner：新 guest/contract case 必须登记映射与 ownership，整进程 exit 非零/timeout 即污染所属项；缺 case、重复 ID、部分循环、SKIP 都不能补成 PASS。提供 24 项 R2 结果与完整 V0 结果，区分 `auxiliary_status` 和要求的 APK 环境。保存所有失败/timeout 与每轮日志，更新 progress/AGENTS/CLAUDE；旧记录不改写。

按逻辑分批提交：协调 gate/请求 → mutator 安全 → cache/range → 持久 owner/runner/证据。每批保留必要的回归，不能只提交新 PASS 数字。需要修改子仓时先按 ownership 用 gh 核对 tencentmalos 自有 repo/ref；当前缺陷优先在主仓 adapter 修复，不要求改变 FEX pin。

## 进入 G3 的门槛

上述 P1 探针均变为拒绝/正确新结果、无挂起；持久双 owner 的两组 100 epochs 通过；完整回归和失败保护通过；所有 source/binary/日志身份可追溯。仍未执行的 APK/ART、WaitingHle、真实 HLE 部分如实 pending。

随后 G3 按真实 typed gate、长度/方向 pin、每线程错误/TLS、两层 InvokeGuest、可取消 WaitingHle 顺序展开；先消除 context 级 unexpected-syscall 错误串值，补 host/guest FP 环境切换。G4 再接普通 APK/Foundation/ART/LLDB，并补 package verifier 的 ABI/closure/ZIP 欠项。16 KiB、finite Step、Vulkan、完整游戏/VR 不扩入本次修复。
