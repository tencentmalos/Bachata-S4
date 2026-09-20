# Android / Orbis 同步原语交接与评估入口

更新：2026-09-20。用户最新要求是先整理当前分支、提交并推送，供另一台电脑的 AI 并行评估。这里区分已交付事实与待验证工作；不要将历史 AGENTS.md 中所有“未提交/未实现”状态当作当前事实。

## 检出与依赖

主仓库现名：`git@github.com:tencentmalos/Bachata-S4.git`（原 `tencentmalos/shadPS4` 会重定向）。开发基线：`feature/malos/hle_vr`。本批实现/测试/证据提交：`5f025529`；交接文档提交位于其后。

```sh
git clone --branch feature/malos/hle_vr --single-branch git@github.com:tencentmalos/Bachata-S4.git shadps4-review
cd shadps4-review
git switch -c codex/orbis-sync-review
```

先阅读本文件和根目录 AGENTS.md、CLAUDE.md，再选择所需 submodule。**不要执行 `git submodule update --remote`**；应使用仓库固定 gitlink。另开评估分支可避免覆盖正在进行的实现。

最小的 metrics 单测不需要 FEX/Foundation/游戏文件：

```sh
cmake -S tests/host_runtime/sync_metrics -B build/sync-metrics -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/sync-metrics
ctest --test-dir build/sync-metrics --output-on-failure
```

需要支持 C++20 `jthread`/`stop_token` 的编译器及标准库（如现代 Linux GCC/libstdc++，或 NDK r29）。旧 Apple libc++ 和 Ubuntu20.04 默认 GCC9 不满足；本机 macOS 使用 Homebrew LLVM23 的 libc++，Android 使用 NDK r29。

运行 domain benchmark/取消测试时：

```sh
git submodule update --init externals/fmt externals/spdlog
cmake -S tests/host_runtime/sync_performance -B build/sync-domains -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/sync-domains
ctest --test-dir build/sync-domains --output-on-failure
build/sync-domains/guest_sync_performance > sync-domain-results.jsonl
```

性能 benchmark 使用 Linux/Android `RUSAGE_THREAD`；macOS 可只编译 `guest_sync_isolation_tests`。新源码直接 include `common/types.h`，不依赖前一个头文件偶然引入类型。

完整 APK 需要 `references/FEX`、`foundation` 等固定子仓及 Android dependencies。Foundation 的 `third_party/profiler_sdk` 地址是 **`git@code.byted.org:spatial/profiler_sdk.git`**，另一台机器可能没有权限；这不是主仓库 push 丢失。没有此权限时先完成源码和独立测试评估，准确列出完整构建阻碍，不使用任意新 SDK 代替固定版本。

游戏/固件、APK/.so、原始大 PROF/KGSL/RDC、设备权限与本机 build 目录均不在仓库。独立未跟踪 `externals/dear_imgui` 不是本工程子模块，未导入。不要把本机绝对路径当另一台机器的可用资源。

## 当前已交付

1. Internal Scale 五档0.25/0.375/0.5/0.75/1.0，默认0.5；低倍率提交0343a73e、aeb087d2已在本分支。按用户最新要求，AYN设备设置已改回0.5、等待下一次游戏启动生效。High1×1/FDM OFF继续保留。
2. Bloodborne固定tick自旋的TSC混域问题和kernel semaphore跨对象广播已在此前提交修复，详见[上一轮报告](../validation/android-native-host/bloodborne-tsc-20260920.md)。不能单独取消FEX缩放而保留HLE频率，或反过来。
3. 新增HLE调用统计、litep汇总counter、详细阶段tag和DebugBus控制，默认OFF。主入口是 `FunctionAdapter::Invoke` / `SyncMetrics::Classify` / `SyncMetrics::Session`。
4. POSIX sem_t与rwlock改每对象CV；POSIX取消选中waiter时转交通知；保留domain元数据锁。Android无关操作32次，等待线程主动切换从33/34降到1/1；Linux同一HLE实现34/33降到1/1。
5. 新增同源Linux x64/Android arm64基准、取消竞态测试、统计单测。最终APK已构建并安装；**游戏中的新增统计采集、Litep读取和最终APK实景回归仍待完成**。

完整说明、命令、counter单位/边界、验证数和原始小型证据见[本轮实现报告](../validation/android-native-host/orbis-sync-metrics-20260920.md)。当前评估最重要的事实是：这些统计是HLE内部含等待elapsed，不是CPU时间，native机制benchmark不是完整桌面Orbis ABI。

## 建议另一位 AI 的工作顺序

### P0：独立代码与证据审查，无设备也可完成

- 检查`guest_sync_metrics.{h,cpp}`、`guest_runtime.cpp`、`diagnostics_commands.cpp`：Session/TLS生命周期、每thread单写者假设、嵌套HLE、start/stop时在途调用、原子快照口径、错误码和返回值分类、counter静态字符串生命周期及关闭路径开销。检查识别别名是否遗漏；未准入HLE不应被宣称实现成功。
- 检查`guest_semaphore.h` / `guest_rwlock.h`：选中后取消、Post/timeout、Destroy忙状态、不同对象隔离、读写优先级和无guest pin跨等待。仍有domain mutex，不能将CV拆分等同完全去串行化。
- 审核bench是否公平、是否覆盖任务目标；区分host domain、完整Orbis ABI、真实FEX三层。已有1/4/12线程/共享/独立场景尚缺生产桌面完整ABI、condition ping-pong、owner被抢占及不同spin预算的专门对照。
- 输出中文评估报告：问题按影响排序并引用文件/行号；区分确定问题、推断、缺失证据，附最小复现/验证方法。不要仅因某段wait很长就断言全局持锁。

### P1：有合适设备后完成数据链路验收

- 用户指定Internal Scale0.5；继续Turnip、High1×1、FDM OFF。进入血源可见诊所/实际场景后才采样，loading不能冒充游戏性能。
- 从`hle_sync status`取得新context；先`start CONTEXT`，抓同一Session两次JSON汇总。再短时间`detail CONTEXT`配合Litep file/ring，确认实际`HLE.Sync.*` counter/tag可被MCP加载、查询并与原HLE scope对应；保留丢包/边界信息。
- 使用现有KGSL/sched sidecar区分CPU/runnable/sleep，不能将聚合elapsed直接当CPU开销。低成本汇总与详细模式做OFF/ON/OFF，确认工具扰动。
- 优先回答：Mutex Lock/Unlock次数及查找/guard/publish比例；Cond等待与重获锁比例；Sema等待频率和长尾；POSIX/rwlock是否真正在血源热点。其他线程等待不是自动可节省的时间。
- 新APK的正常Stop/取消、场景可操作仍需针对性验收。该设备当前无游戏在跑；不要把中间APK到达诊所当最终APK验收。

### P2：按数据分层优化

- 先处理确证的host实现成本：对象查找/checked memory/短domain metadata锁，不新增全局执行锁，不删除必要一致性和映射生命周期检查。
- 再评估**随应用发布的HLE FEX同步快路径**。应放通用runtime/custom SDK，不能放CUSA游戏patch。复用已有compiled guest prototype作研究起点，但它不是production ABI。
- 一份权威同步状态必须同时服务guest CAS快路径、host contended慢路径、condition release/reacquire、递归/错误检查、取消/销毁/VM退役。明确ABA和owner generation，禁止guest/host各持一份互不一致的owner/depth。
- Windows x64 on Android可参考Wine的有限spin、期望值等待和按地址唤醒；NT语义/内核NTSYNC不是Orbis的即插即用替代。遵守FEX子仓自己的AGENTS/CLAUDE要求，先进行只读评估，不顺手升级/改写FEX。

## 其它事项的优先级

- GPU/BufferCache：之前已做区域锁、上传等待移出锁、GPU+host-submit双完成退役。CPU生产者链与GPU77ms级等待仍有未归因部分；本轮用户要求先解决同步原语，不盲目回退epoll或新增GPU全局锁。
- Internal Scale真实总RAM节省、ASTC/BC生命周期复用、scratch pooling等仍有历史待办，按对应报告评估；本轮没有内存收益新结论。
- VR：仍按Android SBS第一阶段、Swan/OpenXR第二阶段；本轮不扩展VR、不启用FDM。普通present保持性能优先，避免多余复制/blit/render pass。
- 用户此前暂停完整游戏回归；进行针对性验证并说明边界。

## 本机状态与可移交性

源机器工作目录为`/Users/bytedance/workspace/emulations/ps4/shadps4`，仅用于辨认原始记录。原始本轮证据在其`build/validation/orbis-sync-20260920`；可跨机器使用的小型文本/JSON结果已提交到`docs/validation/android-native-host/evidence/orbis-sync-20260920`。

AYN Thor原序列号9c2841a4；最终APK3ba6e584 / host914dd36f已安装，当前Settings → Graphics选中0.5，无活动游戏。最后一次真实游戏是中间APK的PID6944/gen1/context1，已普通UI Stop；这些ID不得用于新进程控制。本轮没有新PROF可供远程AI宣称已分析。

KGSL与设备技能入口已在AGENTS.md/CLAUDE.md登记，但`workspace/bug_reports`、`workspace/devices`、`workspace/spatial_mcp_publish`是额外工作区，不保证另一台电脑存在。缺失时仍可完成P0，准确说明P1的环境依赖。
