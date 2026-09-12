# G2 出口关键问题修复（2026-09-09）

基于 `e24aad69e8b174fa9cbebe713918b42838c8ec3f`，分支 `codex/android-fex-round2`；本记录对应尚未提交的主仓修复。FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842` 与 Foundation pin 保持不变。

**结论：本次复核的权限/旧译码、G27 来源链路、M27–M29 虚弱负例和新 case 漏计已修复并通过 CLI 回归。G2 完整矩阵仍待补齐；未宣布 G2/G3 或 V0 最终验收通过。** 原 [e24aad69 收口复核](g2-close-review-2026-09-09.md)和历史 close report 保留，后续以 [交接 spec](../../specs/android-fex-round2-g3-handoff.md)为执行入口。

## 1. 根因与实际改动

### 权限变更不能只改 guest mprotect

FEX 执行的是独立 host JIT 页。原 Reprotect 只更新 guest 页权限及元数据，热 lookup/block link 可以绕过 decode 阶段的可执行范围查询，因此撤销 X 后仍返回旧 A；RW 改字节再 RX 也可能继续执行旧 A。

主仓 `GuestAddressSpace` 的普通 Protect、Unmap 和 token Reprotect 现在统一在修改权限/backing **之前同步调用已注册 sink，退休 shared/local/decoder 译码**。RW→RX 也执行退休，因此调用方遗漏额外 Invalidate 不能留下热 A。回调期间 space 锁释放、in-flight 计数保持，普通写入、新 pin、execution lease 与其他 mutator 被排除。未改变 context→space、解锁后 sink→context→FEX 的锁序。

失效失败或 mprotect/mmap 失败会 poison；释放 token 不会清 poison。授予 X 仍不能绕过已有 poison，恢复须显式失效/发布或受控非执行 remap。Protect/poison 的单 mapping generation 与全局 generation 同步更新。该策略保守，可能增加 permission-only 操作的失效成本；尚未做性能评估。

G28 验证同一线程、两个独立代码 mapping、热 caller 跳入被撤销 X/Unmap 的 target；仅检查 Run 入口 RIP 无法通过此测试。另验证 RW 写 B→RX 无额外 Publish/Clear，原线程执行 B。

### G27：真实 guest 改写实际 target

writer 和 target 在不同 mapping。两个 target owners 全程保留 handle/generation/TID；先运行 A，再在协调 token 下撤销 target X，释放 token 后只调度独立 RX writer，target owners 保持停止。guest 直接向 target store 每轮不同的 immediate（34…43）；host 只读回核验、Invalidate 和提交 RX，不复制 host constant_b。随后两个原 target 线程都返回该新值。

日志保存旧字节 immediate、实际 guest immediate、两个 owner 身份和实际返回。独立负例把 writer 替换为 return_only，其余逻辑相同：target 始终为 17，G27a FAIL、进程 exit 1。这个退出是预期负例，不能并入正例套件统计。

这是 host coordinator 子路径，仍未实现真实 guest→HLE 的 ExplicitPublication。

### G25/G26、Clear 与负例

- G25/G26 初始化独立 marker/counter，保存并消费每个 Run future，逐轮核验 PauseRequested、handle/generation/TID、两个 stopped owners；各 100 epochs。JSONL 保存 stop/token epoch、身份、旧 counter、新 marker/counter 和结果。token 内发布/remap 前后比较所有公开架构字段与两个数据 slot；非输出寄存器/XMM/FS/GS/MXCSR 有非零种子。
- G23 改为显式测试用 RWX 热 mapping，原线程 raw 改 B 后只调用 Clear，不用 Protect/Publish/Remap 替它提前失效。比较完整公开 GPR/RIP/RFLAGS/XMM/MXCSR/FS/GS、停止身份与内存；再次执行 B 后，除有意改变的 RAX 外，寄存器与内存保持。x87/YMM 不在当前有效位/能力声明内。RWX 仅属此隔离测试，不构成长期 RWX 的产品设计。
- M27 在 Publish、Remap、Reprotect 三类被 barrier 阻塞的 sink 回调期间，用**同一有效 token**触发全部 token mutator，以及普通 Protect/Unmap/Write、读写 pin 和 execution lease，均验证 Busy。
- M28 明确为 **syscall 前注入**，核验 ENOMEM/EACCES、失败前后权限/mapping generation/backing marker、释放 token 后执行 lease 仍封锁及 remap 恢复。不能据此宣称 kernel 部分失败 rollback。hook 由 RAII 清理，`V0_BUILD_TESTS=OFF` 构建没有两个 setter 符号。
- M29 使用实际 ReservationSize 构造末端外地址，补 overflow/zero/misalignment，并实际使用 moved-from 原对象验证拒绝和 moved-to token 仍关闭准入；保留 released/foreign 负例。
- G24 从两次扩大为 **合计 100 次**，轮换 Pause/Cancel/Shutdown 在内部 request 前/后的次序，观测 held owner、部分停止、timeout 保留 drain、并发 coordinator 拒绝和 retry 恢复。不是每一种组合各 100 次，也不覆盖 WaitingHle/InvokeGuest。

### Runner 与证据

新增 G25–G28/M27–M29 的 SUITE_MAP 和 ownership，FAIL+exit0 会传播到对应 V0/R2。G25/G26 的 100 个 EPOCH、G27 的 10 个 STORE 必须完整、编号唯一、结构有效且 ok=true；仅总 PASS、缺轮、重复轮或坏 JSON 均失败。新增部署 SHA 校验，校验失败不执行 binary，日志保留实际 deployed SHA；临时路径按运行隔离。

结果 JSON 现在含 **24 个 R2 项**，已运行的映射子检查放 auxiliary，最终项均保持 NOT_RUN。auxiliary PASS 只指列出的子检查，并不覆盖其未列出的完整矩阵。

## 2. 验证结果

环境：Pocket DS / SG8275 / API 33 / ARM64 / 4096-byte 页。NDK target API 35；在本次新建 embedder 目录编译并随修复重编，复用固定 FEX 静态库（assertions OFF），不是 FEX 全量 clean build。未做 Swan/API36/普通 APK 验收。

| 检查 | 最终结果 |
|---|---|
| 真实 FEX guest suite | 97 个独立 ID，195 次检查全部 PASS，exit 0 |
| Android contract | 43/43，exit 0 |
| macOS contract | 42/43 + 1 SKIP（host 拒绝 RWX），exit 0 |
| host HLE 手工 ABI frame | 14/14；不是真实 HLE |
| device page probe、bionic smoke | PASS，exit 0 |
| 原 NX 探针 | mapping_X=0，GuestFault，未返回旧代码；exit 0 |
| 原 rewrite 探针 | 原线程返回 B=34；exit 0 |
| 禁用 guest store 负例 | G27a FAIL、exit 1，符合预期 |
| runner unit / accounting / G1 evidence | 9 个 unit + 既有两套回归全部通过 |
| 无测试 hook 生产 API 构建 | 成功，nm 不含两个 failure setter |
| V0 runner | 10 PASS / 0 FAIL / 47 NOT_RUN，3 deferred |
| R2 runner | 24 项最终 NOT_RUN；9 项具有已映射 CLI 子检查 auxiliary PASS |

完整 [证据索引](g2-exit-repair-2026-09-09/README.md)、[source/依赖/Build ID/部署 SHA manifest](g2-exit-repair-2026-09-09/manifest.json)、[正式结果](g2-exit-repair-2026-09-09/results.json)。195 与 97 的差值来自 G24 重复检查，不能拿 195 与旧 95 直接计算新增用例数。

中途失败保留：新增回归先发现 G26 warm-up 误读前一用例留下的 marker，以及 G10 的故障注入被新的 Protect 失效提前触发。已分别清零 slot、将注入点移回预期 pinned-write 边界。初次失败及第二次通过日志均保留，最终证据使用完成修复后的 source hash。

## 3. 尚未完成与下一步

1. G2 尾项：真实 QueryGuestExecutableRange/decoder 的邻接 mapping、边界和同 VA remap 矩阵；跨 mapping range/销毁后 token 的完整负例；把 G25/G26 每轮记录补到规范所需的 request/ack、mapping/code generation 与耗时，并对矩阵组合逐项计数。当前 G24 合计 100 次不等于每类 100 次。不得回退已经修好的权限/缓存语义。
2. G3 首项 H0：context 级 `unexpected_syscall` 仍存在。先实现 per-thread/per-invocation 错误归属和未注册 syscall/gate 后立即停止，证明后续 sentinel store 不执行、另一 owner 不被污染，再接 typed HLE。
3. 然后完成 typed 参数/返回、长度方向 pin、errno/TLS、每次 crossing 的 FP 保存恢复、两层 InvokeGuest 和可取消 WaitingHle。
4. G4：Swan / Android 16 / 4 KiB 普通 APK/JNI/ART、Foundation lifecycle、package verifier ABI/closure/ZIP、只读 guest snapshot。16 KiB/finite Step/Vulkan/游戏/VR 后置。

本批未 commit/push；上述源文件、测试、关联 spec/报告和历史未跟踪证据必须一起审核后提交，不能只提交本报告。无关 Windows/Vortek、Bachata-S4 子仓本地修改和 externals 临时目录保持原状。
