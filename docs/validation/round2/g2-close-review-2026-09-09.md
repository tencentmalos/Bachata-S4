# G2 收口复核（2026-09-09）

**结论：认可新增实现进展，不接受当前“G2 CLI 机制已收口”的结论。先补权限/cache、真实 guest-store 验证和结果证据，再进入 G3 的执行阶段。** 下一阶段执行 [G2 出口与 G3 入口 spec](../../specs/android-fex-round2-g2-exit-g3-entry.md)。

被审 HEAD `e24aad69e8b174fa9cbebe713918b42838c8ec3f`；已核对 origin 同分支指向相同提交。本次未改生产代码/测试；检查时 `src/`、`scripts/`、`tests/` 与 HEAD 一致。原 [close report](g2-close-2026-09-09/README.md)保留原样。[本次独立证据](g2-close-review-2026-09-09/README.md)包含完整日志、进程状态、源码/binary/deploy 身份与探针。

## 1. 已确认的进展

- `793072f2` 已提交上次关键修复源码/测试。原报告中“未提交”是当时状态；本次复核前工作区 progress/AGENTS/CLAUDE 仍停留在旧状态，未被该批提交同步。
- `d4e4c666`/`b6b7d421` 的 G25/G26 确实在循环外建立两个 TestOwner，每个创建一次 guest thread；每轮切换版本、观察独立 slot marker 和 counter。这比旧 G21/G22 的串行 fresh-thread smoke 有实质进展。
- `ec5469b6` 新增真实执行 store 的 smc_writer；但 G27 尚未形成规范所需发布链路，见 P1-2。
- `8f2a28bc` 增加系统调用包装 seam，能够走 ENOMEM/EACCES 返回路径和 remap poison 恢复；`4e49a7a1` 增加若干 token/range 拒绝测试。覆盖范围应按断言计算，不能称“完整失败矩阵”。
- 独立目录通过 canonical `cmake/fex` 重新编译主仓后，Pocket DS / API 33 / ARM64 / 4 KiB：**guest 95/95、device contract 43/43，均 exit 0**。复用固定 FEX 静态库，未全量重编 FEX；本次未重跑 host/page/bionic，原收口报告中的这些结果不作为本次独立验证。

## 2. 阻塞收口的发现

### P1-1：Reprotect 撤销 X 后仍能执行旧 JIT；无需 HLE 即可复现

[ReprotectUnderToken](../../../src/core/guest_cpu/api/address_space.cpp:1114)成功路径只 mprotect、改 permission/mapping generation，没有失效旧译码，也没有留下待失效的执行门禁。[Run](../../../src/core/guest_cpu/fex/fex_context.cpp:733)取得 execution lease 后可继续进入已缓存 block。guest 映射失去 X 不会自动撤销 FEX 自己的 JIT 页。

本次在同一个热线程上复现两条路径：

| 探针 | 实际结果 |
|---|---|
| warm A=17 → token Reprotect RW → 释放 token → 同线程同 RIP Run | mapping_X=0、poisoned=0，Run 成功 Returned、RAX=17 |
| warm A=17 → token RW → 原始字节换 B=34 → token RX → 释放，无 Publish/Clear | mapping_X=1、poisoned=0，仍 Returned、RAX=17 |

两个探针均用 exit 2 报告契约失败，未挂起。第二条专测此前 spec 要求的“遗漏显式失效不能执行旧代码”；第一条只撤销权限、未写任何代码，也能复现。原 [close report 第 32 行](g2-close-2026-09-09/README.md:32)所称“未失效则拒绝执行已保证旧代码不可达”与实测不符。

该项已在上次 N2 明确列为 G2 未完成工作；本次实现未修复它。live QueryGuestExecutableRange 只能影响实际查询/重新解码，不能替代已热缓存路径的退休。需把权限/映射改变、shared/local/decoder 失效、失败封锁作为完整事务；不能只给 Run 起始 RIP 加检查而遗漏跨 block 跳入撤销范围。

### P1-2：G27 覆盖了 guest-authored bytes，且热代码/线程身份不符合 R2-M04

[G27 第 1886 行](../../../tests/guest_cpu/guest_execution_tests.cpp:1886)先将 guest_bytes 放进 image，但 [第 1899 行](../../../tests/guest_cpu/guest_execution_tests.cpp:1899)又用完整 `fb->bytes` 覆盖 image 的开头；真正 PublishCode 的 B 来自 host fixture。guest 的 store 和五字节比较确实执行过，**它们没有决定最终发布的指令**。

此外：

- A 由 RunConstantOnce 热执行后销毁线程；writer 通过 LoadFixture 覆盖同一个 code_base，已经替换/失效目标 A。
- writer 实际 store 到 data_base，未直接改另一段已热执行代码；host 随后重建整张 B image。
- 最后再次 RunConstantOnce 创建新线程；注释中的“SAME thread concept”不等于复用原 handle。
- 最终 RX 又放在 token 释放后，恢复了此前已要求消除的发布窗口。

因此不能将 G27 PASS 计为 N3 host-coordinator 子路径完成。应使用独立 writer/target 代码区、保持 target owners/handles，guest 直接写 target，协调者仅显式失效/发布该已修改范围；版本每轮变化，日志证明执行结果跟随 guest 实际字节，而不是固定 host 常量。

### P1-3：G25–G27/M27–M29 未接入 runner

[SUITE_MAP](../../../scripts/android/run-v0-tests:190)与 ownership 仍止于旧 G24/M26，M09 还只依赖 G20–G22。没有新增 24 项 R2 结果视图。

本次使用现有 runner 单元测试入口，对 G25a、G26a、G27a、M27、M28、M29 分别注入 `FAIL` 且 suite exit 0，结果全部为：`owned=False`、`mapped=[]`、报告 `failed=0`。这是合成的 accounting 负例，不是声称当前 C++ suite 会在失败时 exit 0。当前 suite 若整体非零退出，会污染已有映射项，但仍不能代表新 case 已有正确归属；缺失新 case 也没有对应门槛。

### P1-4：N4 证据和可交付文档没有完成

`e24aad69` 实际仅新增 6 个文件、45 行：guest.txt 3 行、contract.txt 1 行、bionic.txt 1 行、page.txt 1 行、两个本地 SHA 和 README。它们不是完整原始 suite 输出，缺每 case/每 epoch、独立进程状态、Build ID、部署 SHA、源码/fixture/runner/FEX 库身份以及 24 项 R2/V0 结果。

上次的 g2-next/g2-repair specs、修复报告和证据仍未跟踪；progress/AGENTS/CLAUDE/docs index 有未提交旧改动。HEAD 和远端提交相同不能证明依赖这些本地文档的交接可以从 clone 重现。需明确提交相关文档，保留独立的无关改动。

## 3. 还需收紧的 P2 覆盖

- **M27 的 token 分支不会执行**：publisher 已持有效 token，另一个 Quiesce 按契约返回 Busy；`if (token.Epoch()!=0)` 中的 token mutators 因而跳过。M24 已覆盖同 token Reprotect/Remap，但 M27 没补齐声称的 Publish/Invalidate/Remap callback/draining 全矩阵。用 publisher 的同一个 live token 做 barrier 测试，不能用第二次 Quiesce 代替。
- **M28 检查比描述少**：fault seam 是在 syscall 调用前直接返回错误，不证明真实 kernel 部分破坏/rollback；没有旧 backing sentinel、失败后 permissions/generation 的立即比对，也未在 token 释放后检查执行封锁。生产构建还保留可外部链接的全局 failure setter；建议测试构建限定，并使用 RAII reset。
- **M29 “outside”在 reservation 内**：MakeSpace 默认 64 MiB，测试使用 base+60 MiB，实际是 reservation 内未映射地址；没有 overflow/真正越界/misaligned 矩阵。所谓 released token 已被赋为空 token，不是验证“旧句柄不能解除新 epoch”等生命周期场景。
- **G25/G26 缺逐轮证据与完整状态保持**：新 Run 的 futures 被丢弃，未逐轮记录/核验 RunResult 和 ack；“handle stable”只检查最终有效及两个缓存 TID 不同。没有要求的逐 epoch identity/generation/stop 记录，也没有完整 GPR/FP/guest-memory 保持和 removed-range 查询验证。保留现有持久 owner 正向结果，补齐这些断言，不必退回重写整个测试。
- G24 仍是两种 Cancel 时序，不等于新增 100 轮协调 Pause/Cancel/Shutdown/迟到 ack 矩阵。WaitingHle/InvokeGuest 的实际竞态可按主 spec 留待 G3；纯 CLI 权限/范围和其余已支持路径不依赖 G3。

## 4. 下一阶段顺序

1. 修权限/cache 事务，并把本次两个非零退出探针转成自动回归；验证跨 block、多映射、RW/RX、removed range 与失败恢复。
2. 重写 G27 数据来源和身份链路；补 G25/G26 逐 epoch/状态证据及 M27–M29 的真实负例。
3. runner/24 项 R2/V0/源码到设备身份全部归档，提交主仓所依赖文档，再判定 G2 CLI 出口。
4. G3 从 **R2-H05** 起步：每线程、每 invocation 的错误归属，未注册 syscall/gate 立即停且后继 sentinel store 不执行；然后 typed gate、buffer pin、FP/TLS/errno、两层 callback、WaitingHle Cancel。

Swan/Android 16 普通 APK 可依主 spec 留给 G4；这不是拒绝当前 CLI 收口的理由。当前拒绝来自已经独立复现的 CLI 机制错误与缺失验证。16 KiB、finite Step、Vulkan、游戏/VR 仍后置。
