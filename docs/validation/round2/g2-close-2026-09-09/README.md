# G2 运行中协调事务收口（2026-09-09）

G2（R2-M01–M05 的协调事务机制）在 Pocket DS / Android API 33 / ARM64 / **4 KiB host page** 上通过 CLI 真机验证。**这是 CLI 辅助证据，不是 Swan / Android 16 普通 APK 验收**——R2 最终仍要求 G4 用普通 APK 承载同样用例。分支 `codex/android-fex-round2`。

## 真机结果

| 套件 | 结果 |
|---|---|
| `guest_execution_tests`（真实 FEX x86-64 执行） | **95/95 ALL PASS** |
| `guest_cpu_contract_tests`（device） | **43/43 passed** |
| `host_page_size_probe` | ALL PASS（page=4096） |
| host contract（macOS） | 42/43 + 1 SKIP（M13f 需 RWX host） |

本地 binary sha256 见 [local-hashes.txt](local-hashes.txt)；原始套件输出见 [guest.txt](guest.txt)、[contract.txt](contract.txt)。

## 本轮交付（g2-next spec N1–N4）

基点为 G2 审核修复 `793072f2`（guest 90/90、contract 40/40），之后：

- **N1 R2-M01 持久双 owner**（`d4e4c666` publish、`b6b7d421` remap）：新增 `version_a/b_loop` fixture；G25/G26 让**同一对 native owner、同一 handle/generation/TID** 跨 100 epoch 交替发布/同 VA remap，每轮版本标记翻转且两个独立进度 slot 在新版本下继续推进。
- **N3 R2-M04 host-coordinator 子路径**（`ec5469b6`）：新增 `smc_writer` fixture，由**真实 guest** 写出 `mov eax,34` 字节；G27 验证 host 协调者 ExplicitPublication 后原代码区执行 34，10 次迭代。完整 guest→HLE gate 留待 G3。
- **N2 失败/权限矩阵**（`8f2a28bc` M27/M28、`4e49a7a1` M29）：sink callback 期间所有 mutator 和 read/write pin 全部 Busy（M27）；新增生产默认走真实 syscall 的测试 seam，注入 mmap ENOMEM / mprotect EACCES，验证错误类别、fail-closed（remap 失败后 execute 保持封锁）与失败后恢复（M28）；zero/越界/错配 range、moved-out token、foreign space token 全部拒绝（M29）。

## G2 已有机制（未回退）

协调 stop/drain/commit barrier（QuiesceContext + drain quiesce + execution lease）、token-scoped `ReprotectUnderToken`/`RemapUnderToken`、`ClearCodeCache`（shared + live-thread 失效，覆盖可执行 mapping）、poison/sink drain/失败封锁、BeginDrain 先关闭准入、只消费内部 Pause 保留外部 Cancel、超时 retry 恢复不清除 epoch。

## 仍未关闭（非 G2 范围内）

- **Swan / Android 16 / 普通 APK 验收**：当前为 Pocket DS API 33 CLI。所有 G2 用例需在 G4 以普通 APK 身份重跑才满足目标环境。
- **R2-M04 完整闭环**：guest→HLE 真实发布 gate、两层 InvokeGuest、可取消 WaitingHle 在 **G3**。
- **QueryGuestExecutableRange decoder 缓存矩阵**的细粒度专项（permission-only reprotect 与旧译码交互）部分依赖 G3 的真实 HLE 边界；当前 fail-closed 门禁（未失效则拒绝执行）已保证旧代码不可达。
- 16 KiB / finite Step / Vulkan / 游戏 / VR 不在二周目范围。

## 结论

G2 的运行中协调事务机制——停止所有 owner、token 下 backing/字节/权限变更、shared+thread 缓存失效、失败 poison 封锁与恢复、sink/pin 排他、真实 syscall 失败处理——在 4 KiB ARM64 真机以持久双 owner 100 epoch、remap 100 epoch、guest 发布 10 次和完整失败矩阵闭环。G2 CLI 机制收口，进入 G3 真实 HLE。
