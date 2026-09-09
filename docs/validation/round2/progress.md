# 二周目实施进度（2026-09-09 G1 修复后）

分支 `codex/android-fex-round2`，本次修复基点 `b3bfabceb28b2f65884fd0ccced9b2fc4db8913d`，源码差异与 hash 在 [manifest](g1-2026-09-09/manifest.json)。本轮不改 FEX/Foundation/其他子仓 pin。

**G1 C01–C04 核心已修复，Pocket DS / API 33 / ARM64 / 4096 页 CLI 辅助验证通过。Swan / Android 16 普通 APK 验收未完成；G2–G4 尚未实施，Round 2 / V0 仍在进行。**

- [修复报告](g1-repair-2026-09-09.md)：旧实现的 paused Cancel、ack/epoch、停止态读写和假并发观测问题，以及重建 context 时重复初始化 allocator 的实际故障。
- [当前停止路径](g1-control-decision.md)：JIT 入口 interrupt page → stop-spill → owner Run 返回。Resume 只改变准入；不再使用 SleepThread 停驻 / host SIGILL 恢复。
- [本轮证据](g1-2026-09-09/README.md)：guest 81/81；100 次 pause、请求竞争、迟到 ack、旧 context 拒绝；10 次寄存器修改后实际执行。空 embedder 构建也通过，复用固定 FEX 静态库。
- 原有 contract 回归保持：host 34、Android 34；runner accounting 12/12。V0 runner **10 PASS / 0 FAIL / 47 NOT_RUN，3 deferred**，T01/T02/T03/T05 仅有 auxiliary PASS。
- G0 已修正 B02 独立 ELF 与 APK 的口径及重复 ID 覆盖 FAIL 的问题；**package verifier 尚有 ABI、依赖与 ZIP 对齐覆盖欠项**，见下一阶段任务书。不能因历史“G0 完成”就宣称 APK package 已验证。

下一步直接执行 [G2 任务书](../../specs/android-fex-round2-g2-handoff.md)，再 G3 typed HLE、G4 普通 APK/Foundation/ART。规范仍以 [Round 2 spec](../../specs/android-fex-round2.md) 为准。

历史资料保留：[G0 证据](g0/)、[本次修复前进度原文](g1-2026-09-09/progress-before.md)、[一周目结项](../../baselines/2026-09-08-round1-closeout.md)。旧 70/70 未覆盖已发现的问题，不作为当前 G1 完成依据。16 KiB、有限 Step、Vulkan 与完整游戏/VR 仍按既定范围后置。
