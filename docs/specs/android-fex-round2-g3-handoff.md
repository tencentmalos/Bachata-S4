# 下一轮执行交接：G2 尾项 → G3 真实 HLE

> 2026-09-10：H0–H2已出现部分实现，但独立复核发现立即退出、FP、异常及runner缺陷。新执行入口为 [修复→H3 spec](android-fex-round2-g3-repair-h3.md)，事实见 [H2复核](../validation/round2/g3-h2-review-2026-09-10.md)。本文保留历史要求，未完成G2 Q1–Q3继续有效。

日期：2026-09-09。规范仍是 [Round 2](android-fex-round2.md)和 [V0](android-fex-v0.md)。先读 [当前进度](../validation/round2/progress.md)、[本批修复](../validation/round2/g2-exit-repair-2026-09-09.md)、[原复核](../validation/round2/g2-close-review-2026-09-09.md)。旧 [G2 出口/G3 入口 spec](android-fex-round2-g2-exit-g3-entry.md)保留详细要求，本文件规定修复后的执行顺序。

## 0. 接手状态与交付边界

- 基点是主仓 `e24aad69` **加本地尚未提交的修复**，不能直接 checkout HEAD 后当成同一个基点。核对修复 evidence manifest 的源码 hash、git diff 和子仓状态；本批不修改 FEX/Foundation pin。
- 正例真机：Pocket DS / API33 / ARM64 / 4KiB，guest 97 IDs / 195 checks 全通过，contract 43/43；host contract 42/43 + 1 SKIP。V0 10/0/47，3 deferred；24 个 R2 最终仍 NOT_RUN，9 项有明确范围的 auxiliary。
- 已修：Protect/Unmap/token Reprotect 同步退休旧译码后再改 mapping；失败 poison；G27 实际 guest store、持久 target owners、禁用 store 的失败负例；M27 同 token 排他、M28 syscall 前失败/恢复、M29 真实边界/move；新 case ownership/轮次/部署 SHA。
- 已补：G25/G26 每轮 RunResult、同一 handle/TID/generation、公开架构状态/slot 保持及各 100 EPOCH；G23 独立 Clear 和再次执行后的公开寄存器/内存保持。不能重新用 Protect/Remap/Publish 提前清缓存来“证明 Clear”。
- G24 是**合计 100 次**轮换 6 种请求/次序组合，不是每类 100 次。G27 仍为 host coordinator，真实 HLE 未实现。
- 没有 Swan/API36/普通 APK 结果。4KiB 为当前目标；16KiB、Step、Vulkan、游戏/VR 不在本轮。

## 1. 首个交付：补齐 G2 剩余精确矩阵（Q1–Q3）

### Q1：实际 FEX executable-range/decoder 行为

在主仓 adapter 增加仅测试构建可用的观测 seam，或通过真实 guest 分支和受控日志验证**实际** `FexSyscallHandler::QueryGuestExecutableRange`，不得复制一个假的查询实现来测。

- 两个相邻但独立的 mappings，单独 RX/RW；查询首字节、末字节、刚越界地址；return gate 独立身份。
- 已热 RX→RW，Unmap 后查询返回 empty；同 VA remap 的旧范围/权限不得残留。查询与实际跨 block 跳转结果对应，而非只断言 space 表。
- 不相关 mapping 仍能运行；被撤销 target 不能从热 caller 跳入。permission、decoder-range cache、shared lookup、live-thread block links 作为同一事务验证。
- 保留 G28 两条原回归以及 NX/rewrite 独立 probe。禁止只加 Run 入口 RIP 检查；禁止改 FEX 子仓来规避主仓契约。

### Q2：范围、生命周期与失败的剩余分支

- M29 加跨两个相邻 mapping 的范围、space 销毁后旧 token、移动/迟到释放不打开新 epoch；断言两映射的 permission/generation/backing 均未误改。
- 复用 barrier sink 覆盖普通 Protect/Unmap 发起的回调、注销/draining 与新只读 pin；当前 M27 已覆盖 Publish/Remap/token Reprotect，不要重复做第二个 Quiesce 后跳过分支。
- G24 将 Pause/Cancel/Shutdown × 内部请求前/后分别计数；每个规范要求的交错达到 100 次，保存 request/receipt/stop/token epoch 和结果。明确总轮数与每类轮数，设置与压力规模匹配的外部 watchdog。
- 单独验证 syscall 注入失败后 token 释放、原线程 Run/Resume 拒绝、受控恢复；M28 现有 memory lease 证据与 G10 sink-failure 真机证据不能冒称所有 syscall × guest 路径已完成。保留 RAII 和 OFF 构建无 hook 符号检查。
- 活跃 WaitingHle/InvokeGuest 的交错留到 H3；此时不存在这些状态，不制造占位状态凑 PASS。

### Q3：逐轮证据和提交基点

- G25/G26 在当前 EPOCH 上补规范要求的 mapping/code generation、耗时，以及协调者真实 request/ack 关联；无法从当前 API 读取的字段应增加有版本的只读诊断，不能用 token epoch 冒充 interrupt ticket epoch。
- 保留公开寄存器全字段比较，按需增加重新执行后读取 XMM/FS/GS 的 guest sentinel；与现有 G16 寄存器使用证明合并说明范围，不能把 x87/YMM 声称为已支持。
- 新增子项必须同时有 SUITE_MAP、ownership、R2_MAP 与失败/缺项负例。R2 auxiliary 仅代表列出的子项，完整项的 PASS 仍需对应目标环境。
- 检查并提交本批源码、测试、修复报告、spec、上下文以及此前关联的未跟踪文档/历史证据，形成可 clone 的基点。明确 stage 路径；不混入 Windows/Vortek 调研、Bachata-S4 本地改动、externals 临时 checkout 或 binary。若要推送先验证 owned branch，不推进未推送的 child gitlink。

Q1–Q3 完成后新增 G2 CLI 出口记录。此出口只豁免尚待 G3/G4 的交叉/环境项，不能豁免纯 G2 可测试问题。

## 2. G3 首个实现提交 H0：未注册入口的归属与立即停止

对应 R2-H05。当前 `src/core/guest_cpu/fex/fex_context.cpp` 的 HandleSyscall 只置 context 级 `unexpected_syscall`；注释声称按 frame 记录，但实现没有做到。本批未修改它。

1. 先用汇编 fixture：unknown syscall/gate 后紧跟写 sentinel；另一 owner 独立运行正常 fixture。记录当前行为作为基线。
2. 明确 syscall 边界哪些寄存器已经 spill，实际 guest RIP、RCX/R11 clobber、return gate/dispatcher 退出方式；依据固定 FEX 代码核对，不把任意 host stop 当 guest boundary。
3. 错误记录以 thread generation + invocation ID 归属；不在 context 布尔槽里交换。未注册入口后必须在 sentinel store 之前返回 owner，给出定义的 StopReason/error provenance。
4. 两 owner 并发，至少 100 次错误/正常混合：sentinel 不变、正常 owner 返回正确、不串错；fault 优先于 Pause/Cancel，重建后不继承旧错误。不得跨 JIT 栈抛 C++ exception。
5. 接上合法 typed gate 后，再补“合法 HLE owner 与错误 owner 同时运行”的正式 H05 正例。这一步不能用手工 frame 替代。

**H0 完成标准是实时 guest 执行证据，不是把 bool 换成 map。** 如果退出需要 FEX 变更，先按子仓规则处理；优先主仓 adapter 内实现，禁止默认修改受限制的 reference。

## 3. H1：最小 typed guest→host→guest gate

对应 R2-H01/H06 的基础路径。先做一页设计再实现：注册 operation/signature、gate provenance、状态迁移、owner 和锁序、host/guest FP 保存位置。复用现有 HleCallFrame/CallCursor/typed adapter，保持公开 API 不引入 FEX 类型。

- 真实 guest 传入 8 个整数、9 个 double、混合参数及返回，各至少 10 次；验证寄存器/溢出栈、RCX/R10 区别、栈对齐、callee-saved/red zone。
- host 函数调用计数和日志绑定 thread/invocation；非法 operation/signature 不调用 host 函数。
- 每一次 crossing 保存恢复 host/guest FP 环境及异常标志。现有最外层 Run 恢复 fenv 不能替代它。
- 原有 host 14 个手工 frame 用例继续作单元回归，不能标成真实 HLE。

## 4. H2：长度方向 pin、返回值与 errno/TLS

对应 R2-H02。buffer 描述显式包含长度、in/out/inout、nullable、零长及 checked overflow；整个 native call 持有 pin，异常和错误路径均释放。

- 每类坏地址/权限/长度/overflow/过期 range 至少 10 次；验证 host 调用次数为零、guest sentinel 不变。
- 与另一 owner 的 protect/remap/quiesce 竞争，原 buffer 不失效，结束后能恢复变更。
- int/FP/结构返回与 guest errno/TLS 各归当前 invocation；不共享 context 临时值。复用 Foundation 已启用能力，不引入一套网络/反射实现，也不把尚未启用的 Foundation 模块当成现成依赖。

## 5. H3：受控 InvokeGuest、嵌套与可取消 WaitingHle

对应 R2-H03/H04/H06，并回补 G2 的 HLE 交叉项。

- 普通 Run 继续拒绝重入；InvokeGuest 受 owner、context/thread generation、HleScope 生命周期、depth 约束。明确 invocation 栈、guest stack、return gate、outer/inner snapshot 与资源清理。
- 两 owner 独立 FS/GS/TLS 各 1000 次 HLE；至少 100 次两层 guest→HLE→guest→HLE→guest。检查每层状态及 host FP，不能用一个共享 snapshot 槽覆盖 outer。
- 非 owner、过期 scope、普通 Run 重入、超 depth 均无副作用地拒绝。
- WaitingHle 可取消。100 次 native exception/Stop/inner Cancel/rebuild 混合，Stop≤1秒，无持 context 锁等待 owner/HLE 的死锁，无残留 pin/scope/旧错误，无跨 JIT unwind。
- 把 guest writer→HLE publication→原 target owners 执行新版本接成真实 R2-M04；补失败后 Run/Resume/InvokeGuest 均不能绕过 poison，以及 WaitingHle 与 drain 的实际交错。

## 6. 验证与后续 G4

每一小步独立可审提交，跑对应新回归与现有 guest/contract/runner。归档 source/fixtures/runner/所有链接 FEX 静态库 SHA、binary Build ID、部署 SHA、原始 stdout/stderr/exit/timeout 和逐轮记录；失败日志保留。

G3 CLI 通过后再做 G4：Swan Android16 4KiB 普通 APK/JNI/ART、同 backend 的 host lifecycle、Foundation lifecycle、包 ABI/依赖闭包/ZIP 校验和 LLDB 只读 guest snapshot。最终环境没有执行就保持 NOT_RUN。Native Surface/Vulkan、游戏/PSVR 和 finite Step 仍按后续 V0/产品 spec 安排。
