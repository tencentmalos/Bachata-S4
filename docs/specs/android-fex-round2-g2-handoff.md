# G1 修复后的下一阶段任务书：先实现 G2

> 2026-09-09 G2 复核后，执行入口已更新为 [G2 修复任务书](android-fex-round2-g2-repair.md)。本文件保留设计背景；原 §2 的 M02/M03 编号曾与主 spec 不一致，下表已纠正。原 G21/G22 和 ClearCodeCache 的完成判断被 [复核](../validation/round2/g2-review-2026-09-09.md) 否定。

以 [G1 修复报告](../validation/round2/g1-repair-2026-09-09.md)、[进度](../validation/round2/progress.md) 和 [Round 2 spec](android-fex-round2.md) 为入口。G1 的 C01–C04 已有 Pocket DS 4 KiB CLI 辅助证据；Swan / Android 16 普通 APK 最终验证仍待 G4。不要恢复 SleepThread 停驻、host PC 备份或 20 ms settle。`Resume` 只改变准入，owner 必须再次调用 `Run`。

此任务先交付 **G2 运行中协调事务**，不同时重写 HLE、APK 或 renderer。保持 StepScope=None、ExplicitPublication、4 KiB 当前目标。开始时先检查并保留主仓和子仓工作区；G1 修改若尚未提交，先确认源码与其 manifest 匹配并显式提交本次范围。不要夹带 Windows/Vortek 研究文档、Bachata-S4 本地修改或 externals 临时内容。

## 1. 先固化准入、停止和 token 的状态机

在现有 public API / address space 上增加 context 协调层，避免复制第二套 guest mapping / invalidation 系统：

1. 关闭本 context 的新 Run / CreateThread / Resume 准入，记录协调 generation 和参与 owner 集合。不能只逐个发 Pause，任由之前暂停的 owner 中途恢复。
2. 对所有运行 owner 发请求，按各自 ticket 等待停止；分别记录 timeout、fault 和 cancel。request ack 已离开 JIT，但 Run 尾部的 execution lease 可能尚在释放，必须继续有界等待真正排空。
3. 全部 owner 停止且 execution lease 清零后，取得同一 address space 的 QuiescenceToken。旧 generation / 错误 space / 已释放 token 均拒绝。原先普通 Map/Protect/Unmap 的 Busy guard 保留。
4. token 覆盖 backing/permission 变更、guest byte publication、所有线程共享 cache 的实际失效以及恢复准入的完整事务。不要把 mapping generation 递增或队列入队当作失效完成。
5. 成功提交后才能释放 token 并恢复参与 owner；Resume 后由各 owner 的外层循环重新 Run，从 guest RIP 重新 dispatch，禁止回到缓存中的旧 host PC。
6. 失败和 timeout 不解除未知状态的保护，不把未收到 ack 的 owner 当 stopped，不清掉已有 Cancel，也不允许新 Run 绕过 poisoned range。资源回收不能跨正在运行的 owner。

先写清锁顺序与等待释放哪些锁。禁止在持有 address-space 锁时调用会反向拿 context 锁的 sink；复用现有 sink drain / registration 排他与执行 lease。补充新请求与协调提交/失败的竞争测试。

## 2. 按 R2-M01–M05 逐项交付

| 顺序 | 实现和独立观测 | 最低验证 |
|---|---|---|
| M01 | 两 owner 正运行旧版本；协调停止、发布新版本、全 cache 失效、恢复；两份 guest 输出携带 version | 100 epochs，每轮两 owner 的实际结果均匹配新 version；不可销毁 thread 假装失效 |
| M02 | 整个协调阶段的准入、pin/WaitingHle/迟到请求/sink 排他、timeout 和外部请求保留 | 可控 barrier 与失败注入；无提前写入、外部 Pause/Cancel 不丢 |
| M03 | token 下同 VA remap，复用现有 thread；真实 ClearCodeCache 后状态保留及新译码，执行范围同步 | 100 次；foreign/stale token 拒绝、Clear 独立验证、removed range 查询撤销 |
| M04 | guest 实际写另一段已执行代码，再经协调者 ExplicitPublication 执行新版本 | 至少 10 次；先提供暂停后由 host 协调的路径。与 HLE 的真正接入在 G3 合并验证，不使用假 HLE PASS |
| M05 | 既有 poison/sink/lease 契约和新的 Run/Resume 入口都保持保护 | 原 contract 34 项、guest 81 项完整回归；新增协调失败、迟到 ack、并发请求、部分失效负例 |

每次运行保留 version、request/stop epoch、mapping/code generation、参与 owner 的实际 counter、失败与 timeout。runner 新增 subcase 必须登记 ownership；整个进程失败要污染全部所属项，不能只信此前打印的 PASS。本阶段仍记录 CLI auxiliary，不能提前给 APK 验收 PASS。

## 3. G2 之后的顺序

- **G3**：typed guest gate → 长度/方向 pin → 每线程错误/TLS → 两层 InvokeGuest → 可取消 WaitingHle。先消除现有 context 级 unexpected-syscall 布尔状态的跨 owner 归属风险，并在 HLE/callback 边界正确切换 host/guest FP 环境，再并发测试。不可从任意 native HLE 栈直接 stop-spill 返回。
- **G4**：普通 Activity/JNI，复用同一个 `guest_cpu_fex` CMake target 和 Foundation DebugBus，补真实 reflection/packing 依赖闭包；然后 ART signal/altstack、只读 LLDB 安全点、100 次 session 与 10 分钟 soak。FEX 进程级 allocator arena 允许保持，owned threads/mappings/FD/JNI 必须回到热身基线。
- **APK 打包验收前先补 G0 verifier 的剩余覆盖**：目前按 basename 提取会覆盖跨 ABI 同名 so，NEEDED closure 没按 ABI 隔离；`libc++_shared.so` 在 system 白名单中而未验证随包提供；ZIP 仅检查压缩方式，未检查实际 data offset 对齐；默认 alignment 阈值 0、缺 PT_LOAD 的拒绝也需负例。用伪造/最小 APK 和真实 verifier 做正反例，再给 B02 package PASS。现有 12/12 是 runner 回归，不足以证明这些 package 路径。

如需修改子仓，仍先按 [ownership](../subrepository-ownership.md) 用 gh 验证 tencentmalos 自有 repo/ref，并遵循相应 instructions；当前 G1 不需要改变任何依赖 pin。

交付一个阶段就提交一个可审查的改动集，附源码身份、Build ID、部署 hash、原始日志和结果 JSON。最终仍由 Round 2 的 24 项与规定 app 环境决定状态；有限 Step、Vulkan、完整 PS4 游戏/VR、16 KiB 不在本阶段新增范围内。
