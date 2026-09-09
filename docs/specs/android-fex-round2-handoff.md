# 给执行 AI：Android / FEX 二周目

本任务书保留二周目总体边界。当前 G1 核心修复及 CLI 验证已完成，先读[进度](../validation/round2/progress.md)和新的 [G2 任务书](android-fex-round2-g2-handoff.md)，再继续下一阶段。请直接开展实现、验证和分阶段提交，不要只交一份计划；也不要把历史文档中的未实现要求当成现有代码。

## 先读取、再动代码

1. 根目录 [AGENTS.md](../../AGENTS.md)。检查主仓/全部将改子仓的 status，保留独立本地改动。
2. [一周目结项与完整欠项盘点](../baselines/2026-09-08-round1-closeout.md)。实现基线 `85b57cb25a712823ff721388af0ed1ce641a68fb`，测试证据最初是在 `e3746e36 + dirty patch` 运行；不要倒改历史 JSON。
3. [二周目 spec](android-fex-round2.md)全文：G0–G4 和 24 项 R2 验收定义本次工作边界；[V0 API](android-fex-v0-api.md)定义 API 语义，[V0 验收矩阵](android-fex-v0-acceptance.md)保留更大的待完成范围。
4. [事务加固报告](../validation/v0/transaction-hardening-2026-09-08.md)、[poison 后续](../validation/v0/followup-poison-2026-09-08.md)和[对应审核](../validation/v0/review-poison-2026-09-08.md)。已有 poison/lease/sink 排空测试不能退化。
5. [子仓归属](../subrepository-ownership.md)与 [Foundation 接入](../foundation-integration.md)。开始子仓修改前，用 `gh` 验证自有 repo/ref，再读该子仓的适用 instructions。

从包含本任务书的主仓提交建立 `codex/android-fex-round2`（存在时先核对，不重置）。记录起点 SHA；独立 checkout 不依赖当前机器未提交的 Windows/Vortek 文档、SG8275 子仓修改或旧 build 目录。保持原 `codex/android-fex-v0` 作为已推送的一周目基线。

## 依赖检查

当前应使用 FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`，Foundation `1f7008848736b7c6c0779220480344fd5b5fc5e3`。不要按旧 V0 初稿退回 FEX `f2b679f6…`，不要 `submodule update --remote`。

可用以下命令核对现有 ref（输出是当次远端值，不能覆盖主仓 gitlink）：

```sh
gh api repos/tencentmalos/FEX/git/ref/heads/feature/malos/host-page-size --jq .object.sha
gh api repos/tencentmalos/foundation/git/ref/heads/codex/shadps4-android-fex-v0 --jq .object.sha
gh api repos/tencentmalos/Bachata-S4/git/ref/heads/codex/android-fex-v0-frontend --jq .object.sha
gh api repos/tencentmalos/Bachata-S4/git/ref/heads/codex/android-fex-v0-arm64 --jq .object.sha
```

如确需 FEX/Foundation 修改，在已核对的自有 fork 从当前 pin 创建独立 `codex/android-fex-round2` 分支，并 push 后用 `gh` 验证 ref 再动源码。FEX 仅限已有授权的自有 fork、不回流上游；不通过删规则来取得授权。子仓提交先推送，验证远端含该 commit，再更新主仓 gitlink。Android reference 默认只读迁移来源，主仓新增验证 app；无需为读源码改 reference pin。

## 按顺序实施

- **G0**：先修验收的 artifact/environment/coverage 口径。特别是 B02：独立 ELF 不能算 APK 全库验收，版本表/对齐须真实检查。复现现有 contract、guest 和 runner 回归，形成 clean build 锁与命令。
- **G1**：先在 pinned FEX 上写明异步 kick→spill→owner 返回的源码路径，再实现 RequestInterrupt/WaitStopped/epoch/快照。两 owner 真并发、已热身无 HLE loop 100 次暂停恢复，停止每次 ≤1 秒。
- **G2**：实现 context quiesce 和 token mapping transaction，两个运行 owner 的 100 epoch 发布、同 VA remap、ClearCodeCache 与失败封锁。普通 mapping Busy guard 不得删除。
- **G3**：接真正 guest gate、typed ABI、长度与 pin、线程 TLS、两层 InvokeGuest、异常和可取消 WaitingHle。未知 syscall/gate 不得继续执行后续 store，不能共用 context 布尔错误状态。
- **G4**：普通 Activity/JNI + 同一 CMake backend target + Foundation DebugBus/真实 reflection/packing 闭包。最终 CPU/HLE/VM 用例在 app 身份下运行，补 ART signal、LLDB 安全点、100 次清理及 10 分钟 CPU soak。

构建入口仍为 `cmake/fex` 与 `scripts/android/build-fexcore-android`。增加 app 入口可调整 CMake 组织，但不能复制第二套 FEX 编译/链接配方。当前实际 native target 是 API 35；SDK 36 设备不等于已用 API 36 NDK sysroot，锁文件必须区分。

本轮目标始终 **Swan / Android 16 / ARM64 / 4096-byte host page**。UI 用普通控件显示真实 guest/HLE 输入输出即可；StepScope=None、TCP=false。有限 Step、Vulkan Surface、完整 PS4 游戏/音频/VR、16 KiB 设备适配均非本轮交付门槛，也不能填成已通过。

## 完成时提交什么

1. G0–G4 的实现提交、必要的子仓提交与主仓 gitlink，说明迁移源和关键技术决定。
2. 可从独立 checkout 构建的 debug APK、依赖锁、SHA/Build IDs、匹配符号位置、安装/运行/证据收集命令；产物不进源码 Git。
3. `docs/validation/round2/` 下的 24 项 `round2-results.json`、完整 `v0-results.json`、原始日志、全部失败/timeout、延迟分布、app 资源和 LLDB 证据。
4. 更新 AGENTS/CLAUDE/docs 索引与实施报告。明确哪些是代码、host/CLI 辅助验证、app 实测，以及剩余欠项。

只有规定的 24 项及 G0–G4 全部满足才是 ROUND2_ACCEPTED；此时仍可能且预计为 V0_IN_PROGRESS。工具链/设备缺失时保存 NOT_RUN 和原因并继续独立可做部分，不伪造通过、不因 16 KiB 延期停止。
