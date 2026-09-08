# 发布事务接通与可信映射（2026-09-08）

**结论：P1-C 已关闭。公共 `GuestAddressSpace` 发布路径现在真正到达 FEX 的译码缓存；同一探针在同一台 Swan 上从执行旧代码（rax=17）变为执行新代码（rax=34）。状态仍为 `V0_IN_PROGRESS`：审核任务表第 5、6 步（运行线程停止恢复、真实 typed HLE 与 app）未开始。**

基线主仓 `e69aaba9731eba385e5866ec435ee71aa93dbbb5`，FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`（`feature/malos/host-page-size`，未改动）。设备为 Swan / Android 16 / ARM64 / 4 KiB。

本轮对应[发布事务审核](review-publication-2026-09-08.md)任务表的第 3、4 步，并修正审核第 2.4 节点名的两处过度声明。

## 1. P1-C：公共发布路径与真实失效脱节

### 复现

审核记录的现象是两个同名 `InvalidateCode` 含义不同。用[独立探针](followup-publication-2026-09-08/p1c_probe.cpp)复现，只调用公共内存 API，不碰 `CpuContext`：

| 观察值 | 修复前 | 修复后 |
|---|---|---|
| `P1C_publish_accepted` | 1 | 1 |
| `P1C_invalidate_accepted` | 1 | 1 |
| `P1C_generation_advanced` | 1 | 1 |
| `P1C_executed_rax` | **17（旧 A）** | **34（新 B）** |
| `P1C_stale_code_executed` | **1** | **0** |

两次都是 `code_generation 1 -> 3`、两个调用都成功——这正是问题：成功和 generation 前进都不代表旧译码不可达。日志见 [before](followup-publication-2026-09-08/p1c-swan-before.txt) / [after](followup-publication-2026-09-08/p1c-swan-after.txt)，两者用同一份探针源码，分别链接 `e69aaba9` 的 API 源码和本次工作区。

### 修复

在公共 API 层声明 `CodeInvalidationSink`，由 backend 注册；`PublishCode` 与 `InvalidateCode` 都经它到达 FEX 的 `InvalidateCodeBuffersCodeRange` 及每线程缓存。

几个刻意的选择：

- **不复用 `MemoryObserver`**。它返回 `void`、契约上是 best-effort；丢弃译码不是。sink 失败必须让发布失败，否则就是在旧 block 仍可达时报告成功。
- **sink 在 space 锁之外调用**。锁序是 context → space（backend 持自己的锁读 `CodeGeneration()` 生成 snapshot），从 space 锁里回调会反向。token 是这段无锁窗口的排他依据；返回后重新校验 token，因为期间它可能已被释放。
- **失败后撤销 execute**。字节已经改了，此时保持可执行意味着旧译码会对着不存在的代码运行。`RevokeExecuteLocked` 去掉 execute 并置 `code_poisoned`，`PublishCode` 返回 sink 的错误。generation 仍然前进：字节确实变了，之前捕获的 generation 不能继续相等。
- **第二个 backend 注册被拒**，而不是静默替换——被替换的 backend 的译码将再也无人失效。

`guest_cpu_api` 仍不含任何 FEX 头（B05 检查通过）。

### 回归

- `G09a`：**100 次**只经 `GuestAddressSpace` 的 A/B 交替发布，每次执行新常量。这是审核第 4 步要求的证据形态。`G09b` 覆盖 pin 改字节 + `InvalidateCode` 的另一条公共入口。
- `M13f`：sink 失败后范围不可执行、`code_poisoned` 置位、generation 仍前进。

关于 `M13f` 的一处**平台反直觉事实**：macOS 拒绝 RWX，走 skip 分支；**Swan 接受 RWX**，真正执行 execute 撤销断言。所以这条的实际证据来自设备，不是 host。skip 时显式打印说明，不留一个什么都没检查的 PASS。

## 2. 修正两处过度声明

### M13 曾在缺少两线程竞争证据时记 PASS

M13 的验收标准是"pin 的 HLE span 与另一线程 unmap/protect 竞争"。原有 `M13`/`M13b`/`M13c` 全部单线程，套件里没有任何 `std::thread`——审核已指出这条缺失，但 M13 仍记 PASS。

新增 `M13g`：两个 host 线程，一个持 pin 并在整个竞争期间写入，另一个 unmap + protect。断言 span 在持有期内不失效、竞争失败必须是定义好的 `Busy`、释放后范围重新可操作。

第一版把重叠交给调度：macOS 190/200 轮竞争，**Swan 只有 3/200**——一个几乎不竞争的竞争测试。改为 racer 等待 pin 真正建立后再动手，并**断言 200/200 轮都竞争到**，否则失败。这样"测试退化成不竞争"本身会被发现。

### M07 的证据是跨 API 拼接

审核指出 M07 由 G08（走 `CpuContext`）和 host M07（走 `GuestAddressSpace`，不执行）拼成，两者都通过时公共路径仍是坏的。现在 M07 额外要求 `G09a`–`G09d`，即不触碰任何 `CpuContext` 方法的发布证据。

映射是真的生效，不是装饰：用一个把 `M13g` 打成 FAIL 的合成 suite 验证，M13 判 **FAIL** 并写明 `failed sub-cases: M13g`。

## 3. 可复现的构建入口（审核第 3 步）

审核 host 运行用的 CMakeLists 写在临时目录，已不存在，仓库里无法重建——runner 因此找不到 host 二进制，把三个套件标为 UNUSABLE。这本身就是第 3 步要解决的脆弱性。

新增 [tests/guest_cpu/CMakeLists.txt](../../../tests/guest_cpu/CMakeLists.txt)，独立于桌面 emulator 构建三个 host 套件：

```
cmake -S tests/guest_cpu -B build/v0-host -DCMAKE_BUILD_TYPE=Release && cmake --build build/v0-host
```

刻意不挂到桌面 `tests/`：那会让每次 V0 运行都依赖配置整个 emulator。Android 侧仍由 `scripts/android/build-fexcore-android` 手工链接，把 backend 也收敛成 target 是第 3 步剩余部分。

## 4. 本轮结果

| 套件 | 结果 |
|---|---|
| host contract（macOS） | 26/26 |
| host HLE ABI（macOS） | 14/14 |
| host page size（macOS） | 22/22 |
| device contract（Swan） | 26/26，exit 0 |
| device guest execution（Swan） | 41/41，exit 0 |
| device bionic smoke（Swan） | 12/12 |

验收矩阵 **21 PASS / 0 FAIL / 37 NOT_RUN，58 项在范围内，2 项 `DEFERRED_BY_SCOPE`**——与上一轮数字相同，但 M07 和 M13 的依据换成了真正满足其判据的证据。[results.json](followup-publication-2026-09-08/results.json)、[设备 contract 日志](followup-publication-2026-09-08/device-contract-swan.txt)、[设备 guest 日志](followup-publication-2026-09-08/device-guest-swan.txt)。

## 5. 未做与未验证

- **审核第 5、6 步未开始**：运行中线程的停止与恢复（T02/M09）、真实 typed HLE 的 span lease 与 callback、JNI/app。
- **TSan 未能验证 `M13g`**：本机 TSan 连一个最小 `std::thread` 程序都直接段错误（exit 139、无输出），与被测代码无关。这条竞争测试没有经过数据竞争检测器，只有断言覆盖。
- **`M13g` 不覆盖 guest 执行期竞争**：它测的是 host 线程之间的 pin/unmap/protect，不是 guest 正在跑时的竞争，后者仍需 T01/M09 的能力。
- **`code_poisoned` 是空间级布尔**，不是按范围记录。当前只有一个发布范围的场景够用；多范围并发发布需要按范围跟踪。
- Android 侧 backend 仍是脚本手工链接，未收敛为 CMake target。
