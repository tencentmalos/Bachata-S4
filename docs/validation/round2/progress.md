# 二周目进度（2026-09-10 N12复核反例已修补，N3 C0/C1 探针已真机验证）

> **本轮（在 `dfb759a0` 之上）已按 [N12修补与N3实验spec](../../specs/android-fex-round2-g3-n12-fix-n3-probe.md)
> 关闭 A/B 反例并交付可运行的 N3 C0/C1 探针**：`2ebc4892`（A runner 整体失败覆盖直接 checker +
> 损坏产物）、`da803e3f`（B 票据化 gate 状态机 + host 协议负例）、`fa41c657`（N3 C0/C1 syscall 立即退出
> 探针真机通过）。N3 C2 矩阵与 N4 错误归属/正式 G33+ 仍是下一单元；独立探针通过≠生产 R2-H05 验收。
> origin 仍 `0e10defc`，这些提交仅本地未 push。以下保留被审基点 `dfb759a0` 的复核记录。

分支 `codex/android-fex-round2`，被审HEAD `dfb759a0`。本轮提交 `fd1c8a86`（N1）、`911be847`（N2 gate）、
`97f149af`（ENOEXEC）、`6e7f46e5`（进度）、`dfb759a0`（N3设计）。origin实时仍为 `0e10defc`；
H2与后续改动只在本地，相关spec/报告/证据还有untracked/dirty，不能宣称远端可完整检出。FEX/Foundation pin未改。

**N1/N2有实际进展，尚未全面收口；N3没有可执行探针，H3入口不接受。** 先读
[本轮复核](g3-n12-review-2026-09-10.md)，执行[修补与N3实验spec](../../specs/android-fex-round2-g3-n12-fix-n3-probe.md)。
上一[exit-next](../../specs/android-fex-round2-g3-r0-exit-next.md)的N4和[完整修复spec](../../specs/android-fex-round2-g3-repair-h3.md)
的R2/R3/S0–S3继续有效。

## 本次核验

- G30–G32 R2-only FAIL+exit0已改为runner非零，原P1关闭；现有Python18/18、accounting19/19通过。
- 新P1：B02/B05/B06直接checker分别FAIL时，summary.failed=1但overall.has_failures=false、runner返回0；
  新overall只OR sub/suite，漏掉直接父项结果。损坏可执行文件还被ENOEXEC误判wrong architecture/never_started。
- N2等待点在running/lease已登记、相关锁作用域结束、JIT之前，替代任意PC冻结的方向成立。
  但实际gate反例证实旧代未Exited即可Arm新代、一次owner退出确认两次Release；timeout恢复也不完整。
  这些是test-only协议问题，不能描述为本次发现生产Pause故障。需票据/代次、初始化发布和timeout收尾。
- 独立新目录NDK重编，AYN Thor/API33/ARM64/4KiB完整guest104IDs/202checks全过，device contract43/43、
  host42/43+1SKIP；host ABI14/page22、device bionic smoke12通过。canonical为V0 10 PASS/0 FAIL/47 NOT_RUN、
  3延期，R2正式24项NOT_RUN。详见[证据](g3-n12-review-2026-09-10/README.md)。
- tests=OFF构建中gate相关符号为0；tests=ON为12。仅设CMAKE_BUILD_TYPE=Release不会排除test hook。
- 历史11轮文本全过，canonical有部署hash但标fd1c8a86+dirty、无完整source patch，不能倒填为当前HEAD。

## 下一步

1. A：直接checker失败/启动失败进入整体退出，补真实进程与结果一致性负例。
2. B：修补gate有票据的Arm/Release/Exited/timeout状态；补同owner先执行再hold、每轮epoch/identity/耗时记录。
3. C0：N3安装只转发的ARM64 ABI wrapper，保存/核验frame与原obj/func；先做无行为变化实验。
4. C1/C2：C++清理正常退回wrapper后，再实验非spill stop出口；unknown/rejected/合法HLE/错误后loop各100次，
   记录PC/sentinel/pin/host栈/FP/lease。固定dispatcher出口不自动恢复SP，不复制G1的stop_spill路径。
5. D：完整交付源码/探针/构建/日志/JSON/上下文。N3独立实验通过后，仍需N4生产整合与错误归属矩阵。

H0/H1/H2仍为partial：frame→thread fault flag、真实6/8整数和4double gate、bounded pin/sum60。
unknown/rejected后继store、native FP环境与exception故障尚未改；N3本轮只有设计。之后N4→R2（FP/异常/errno）
→R3（完整ABI/buffer/typed registry/TLS）→S0–S3（callback/scope/两层嵌套/WaitingHle）。主仓尚无H3实现，
普通HLT gate不等同FEX callback返回协议。

G2 Q1–Q3保持NOT_RUN；保留publication/decoder失效、poison、真实guest store、persistent owner/state、
failed-drain retry、内部Pause退休和sink/pin排他。Swan Android16/4KiB普通APK/JNI/ART/Foundation生命周期、
package ABI/closure/ZIP/只读guest snapshot仍在G4。V0_IN_PROGRESS；16KiB/finite Step/Vulkan/游戏/VR后置。
