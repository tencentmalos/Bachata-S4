# 发布事务增量复核、修复与验证（2026-09-08）

**本轮已直接修复复核中发现的事务缺口，并完成 Swan 4 KiB 验证：contract 34/34、真实 guest 45/45。状态仍为 V0_IN_PROGRESS。完整验收按实际覆盖调整为 11 PASS / 0 FAIL / 46 NOT_RUN，57 项在范围内、3 项延期。**

起点为主仓 `e3746e36`；FEX 保持 `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`，未修改子仓。本轮代码、测试和文档是未提交工作区改动，没有 push。前置记录：[上一轮后续](followup-poison-2026-09-08.md)、[poison 审核](review-poison-2026-09-08.md)。历史记录保留原样。

## 1. 复核结果与本轮修复

上一轮的执行 lease、poison 封锁、sink 排空、CMake target 及 runner 对非零退出/SKIP 的处理均已有实现；但不能据此认定所有事务边界已完成。

| 发现 | 本轮处理 | 验证 |
|---|---|---|
| CreateThread 把 admission 放在短局部块，实际创建前 lease 已释放 | 将 lease 保留到整个 CreateThread 返回 | 源码生命周期修正，设备创建/执行与既有拒绝用例无回归 |
| 第二次失败直接覆盖单一 poisoned_range；只修第二处即可错误解锁 | 保留多个失败区间，只删除被本次成功修复完整覆盖的区间 | M13k：两个失败分别修复，第一处未修复时仍拒绝执行 |
| InvalidateCode 失败假定“没有改字节”，忽略先通过 pin 改写的 caller | 失败也保守记录 poison、推进 code generation，直到成功失效 | M13l；真实 FEX G10a–d |
| sink 抛异常会泄漏 in-flight count，使 Clear 永久等待，且写后未封锁 | 回调边界捕获异常并转 BackendFailure，重新持锁释放计数，进入失败封锁 | M13m，异常不外逃且 Clear 能完成 |
| token 排除执行，却不阻止无 token 的映射变更插入 | Map/Unmap/Protect/RegisterAlias 在执行、发布或排空期间返回 Busy，不改映射状态 | M13n：活动事务的 Protect、执行中的 Unmap 均拒绝 |
| Clear 排空期间槽位已空，另一个 sink 可被提前注册 | 明确 draining 状态；排空期间拒绝注册、发布和执行准入；完成后才允许新注册 | M13o：两线程旧回调排空期间 replacement 返回 Busy，旧回调不提交 |

M13k–n 在修复前均明确失败，见 [before.txt](transaction-hardening-2026-09-08/before.txt)。M13o 是随后加入的回归，没有将其描述为同一轮修复前实测。

新增真实 guest 故障注入 G10：先缓存 A=17，使用 writable pin 改成 B=34，并注入失效失败。验证 poison 拒绝 execute 权限和实际线程创建/执行准入；恢复真实 FEX sink、成功失效后才允许执行 B=34。没有只看标志、权限或 generation 就宣称旧 JIT 不可达。见[设备 guest 输出](transaction-hardening-2026-09-08/results-suites/guest_execution_tests.txt)。

代码入口：[address_space.cpp](../../../src/core/guest_cpu/api/address_space.cpp)、[fex_context.cpp](../../../src/core/guest_cpu/fex/fex_context.cpp)、[contract 测试](../../../tests/guest_cpu/api_contract_tests.cpp)、[guest 测试](../../../tests/guest_cpu/guest_execution_tests.cpp)。相关差异亦归档为 [source-fix.patch](transaction-hardening-2026-09-08/source-fix.patch)。

## 2. runner 与构建闭环

`run-v0-tests` 本轮继续修正了此前尚未完成的覆盖口径：

- C01 flags、C02 MXCSR/FP、D05 完整非法指令、真实 HLE、实际 guest memory fault、alias 翻译、联合 observer、异步控制和 fault 归属等，不能由目前的部分 fixture/手工 frame 自动升为完整 PASS。保留 `auxiliary_status=PASS` 与已运行的子项证据，完整验收项仍为 NOT_RUN，并给出缺失条件。真实 FAIL 仍优先。
- M13 把 M13h–o 也纳入必需子项；M07 纳入真实失败恢复 G10a–d，不遗漏刚增加的回归。
- B04、M02、M03 共 **3 项**按 scope 延期，不依赖是否运行或是否 PASS。旧记录的“2 deferred / 58 in scope”漏掉了 B04。当前是 57 项在范围内。
- 取消固定的“本次设备全部通过”解释，改为本次观测摘要；每个 suite 保存独立原始日志、进程状态及日志 hash，各验收项引用相应日志。构建路径是观察值，日志已随本报告归档。
- 新增 [runner 自动测试](../../../tests/guest_cpu/test_v0_runner.py)：未启动、无输出失败/timeout、SKIP、设备补足 host SKIP、FAIL+缺项、部分语义覆盖，共 6 个测试，全部通过。

因此本轮由旧 **21 PASS** 调整为 **11 PASS**，是纠正完整验收口径，不是 guest 执行回退。原有能力及新增回归均成功运行。

构建脚本不再保留第二套手工 link_probe 列表：`scripts/android/build-fexcore-android` 构建 FEXCore、生成 fixture 后，调用既有 `cmake/fex` 的 `guest_cpu_fex` 与测试目标；原 runner 入口路径只接收 CMake 产物副本。canonical 产物位于 `build/fexcore-android/v0`，带 Build ID，可匹配符号。FEXCore 仍在独立 CMake 工程构建。

## 3. 验证与复现

| 验证 | 本轮结果 |
|---|---|
| macOS host contract | 33 PASS、1 SKIP，共 34；M13f 的 RWX 场景在本机跳过 |
| Swan contract | 34/34，exit 0，包含 M13f 与两线程回归 |
| Swan guest | 45/45，exit 0，包含 G10 的真实 JIT 失败恢复 |
| host typed ABI | 14/14；仍是手工 frame |
| host page probe | 22/22；不代表 Android 16 KiB 验收 |
| Swan bionic 初始化 | 12/12 |
| Python runner | 6/6 |
| 完整验收 | 11 PASS / 0 FAIL / 46 NOT_RUN；3 DEFERRED_BY_SCOPE |

NDK 本地安装 29.0.14206865，实际 target API 35，设备 Android 16 / SDK 36 / 4096-byte pages。host 是新构建目录；FEX 本体增量构建，backend/测试由 CMake 构建。不是从全新 checkout 验证整个依赖闭包；没有 APK/ART、TSan、游戏或 renderer 验证。

复现入口：

```sh
cmake -S cmake/fex -B build/v0-host -G Ninja -DV0_ENABLE_FEX=OFF
cmake --build build/v0-host
scripts/android/build-fexcore-android
python3 tests/guest_cpu/test_v0_runner.py
scripts/android/run-v0-tests --build-dir build/v0-host \
  --fex-build-dir build/fexcore-android/v0 --serial <device-serial> \
  --out <results-path>
```

[results.json](transaction-hardening-2026-09-08/results.json)、[原始 runner 输出](transaction-hardening-2026-09-08/final-runner.txt)、[证据与 Build IDs](transaction-hardening-2026-09-08/evidence.json)可用于核对本次产物。日志路径在归档 JSON 中改为仓库相对路径；二进制路径保持当时观察值。

## 4. 尚未完成、下一步应推进的内容

本轮完成的是新增复核缺口的修复与验证，**不是完成全部 V0**。

1. **运行中中断与停止回执**：当前 Quiesce 仍是 Busy 拒绝运行线程的限定实现。下一步做无 HLE 循环的异步请求、owner stop epoch、安全点、超时取消和恢复，完成 T02/M09。不得持 CPU/VM 锁等待 owner；不能用 kill 代替正常 stop。
2. **真实 HLE 与 callback**：长度关系、调用期 span lease、ABI 寄存器与栈、两层回调仍需接通；现有 typed adapter 单测不是 guest→HLE 的完成证据。
3. **app/JNI/Surface 与调试**：接入统一 backend target，先同一 fixture 的 app 内加载/执行与停止，再做原生 Surface 的前后台/销毁恢复、ART 信号共存、LLDB 安全点和有限 Step。

当前映射变更策略是保守拒绝：活动执行 lease 或 publication 期间，普通 Map/Unmap/Protect/RegisterAlias 返回 Busy。尚无携带事务 token 的重映射接口；后续 loader/HLE 需要的重映射应作为事务操作设计，不应删除准入检查来绕过限制。pin 的直接写入仍是 caller 按 ExplicitPublication 协议管理的路径；不是透明 SMC。

ExecutionLease 仍遵循 address space 必须比 context 活得久的现有契约。sink 注销不能在自己的 DiscardTranslations 回调内部调用；该自等待前置条件仍适用。Foundation 继续复用现有基础设施，不为后续网络/反射另造框架。
