# V0 执行入口已更新为二周目任务书

请将 **[Android / FEX 二周目执行任务书](android-fex-round2-handoff.md)** 交给下一位执行 AI；其配套 **[二周目 spec](android-fex-round2.md)** 定义当前 G0–G4 和 24 项交付条件。

旧任务书中的 16 KiB 目标、FEX `f2b679f6…` 初始 pin、从零建立 backend 的指引已经过时，因此此入口改为转交当前任务书。旧文本保留在 Git 历史中，不应继续作为执行指令。

当前实际起点为主仓 `85b57cb25a712823ff721388af0ed1ce641a68fb`、FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`，目标 Swan / Android 16 / ARM64 / **4 KiB**。从包含新任务书的提交建立二周目分支，保留 [a7128893 历史基线](../baselines/2026-09-07-android-fex-foundation.md)。

[一周目结项与全部欠项](../baselines/2026-09-08-round1-closeout.md)区分已提交代码、host/CLI 辅助证据和未完成的 app 验收。完整 [V0 总 spec](android-fex-v0.md)、[API 契约](android-fex-v0-api.md)、[验收矩阵](android-fex-v0-acceptance.md)仍定义最终目标；二周目通过并不等于 V0_ACCEPTED。
