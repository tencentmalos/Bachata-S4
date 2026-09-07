# V0 验收矩阵与交付证据

所属：[总 spec](android-fex-v0.md)、[API 契约](android-fex-v0-api.md)。以下均为**待实现、待运行**的验收要求，不是已通过结果。

## 1. 运行环境和通过规则

| 环境 | 要求 | 可以证明什么 |
|---|---|---|
| HOST | macOS/Linux 构建机，必要时 x86-64 oracle | 编译、公共 API/纯数据测试、fixture 独立预期；不能证明 ARM64 FEX 执行 |
| A16-16K | Android 16/API 36、ARM64、**真实 16 KiB 的物理设备**，普通 app 身份 | V0 最终 CPU/JNI/内存/Surface 门槛 |
| A16-4K | Android 16/API 36、ARM64、4 KiB，实机或明确标注的 ARM64 Android 虚拟设备 | 相同 APK 的 flexible-page 回归；不作实机性能结论 |
| A16-16K-EMU | 可选 ARM64 16 KiB Android 虚拟设备 | CPU/VM 预验证；不能替代上面的最终物理设备项 |

设备为 16 KiB 时，`adb shell getconf PAGE_SIZE` 与**app 内** `sysconf(_SC_PAGESIZE)`/`getpagesize()` 必须均为 16384；记录 app ABI 和系统 API。不能只凭 Android 版本、安装成功或 ELF 对齐判定。关闭验证 app 的 page-size backcompat，并记录有效配置；不为测试全局改变设备策略。[Android 官方页大小测试说明](https://developer.android.com/guide/practices/page-sizes)

所有表内用例默认 MUST。除表内明确的条件项目，SKIP/NOT_RUN 不可折算成 PASS。A16-16K 不可取得时，交付状态最高为 V0_IMPLEMENTED_DEVICE_PENDING。某项实现仍失败时使用 V0_IN_PROGRESS 或有实际阻断证据的 V0_BLOCKED，不能使用 DEVICE_PENDING 遮盖已知实现失败。

CPU/API/ABI/VM/控制案例在 A16-16K 和 A16-4K 运行**同一个 APK**；显示和 Android 生命周期至少在 A16-16K 实机执行，4 KiB 环境也需 app 启动/停止 smoke。HOST 结果独立列出。

## 2. 用例设计和 oracle

- fixture 由受版本控制的 x86-64 汇编/C 源生成，保存工具版本、编译参数与字节 hash。不可使用下载游戏、libc/rootfs 或远程程序作为唯一测试源。
- 以固定预期结果、手工推导的不变量、或独立 x86-64 native oracle 对照。预期值不能通过同一 FEX 执行路径生成再比较。
- 未定义的 x86 flags、NaN payload 等按架构有效位比较；不要因为 oracle 的未定义值恰好一致而声称支持。
- 不用只有“函数返回 true”的 stub、读取源码关键字、日志包含 Running 或一个宿主常量加法代替 guest 执行。
- 每个案例验证 guest input/output memory 和关键寄存器；运行时报告确认所用 backend=FEX、实际 FEX revision、guest-code hash。
- 正常 fixture 单例默认 10 秒 timeout；loop-stop 测量另见控制项。预期崩溃的 signal 负例可在独立测试进程执行，该进程仍须由 Android app 启动并有 ART/app 身份；这不改变正常验证 app 的同进程架构。
- 原子/并发压力案例可在运行前声明每个参数组合最多 120 秒的 timeout；实际 timeout 写入报告。不得在失败后只扩大次数/超时阈值并丢掉前次结果。10 分钟 soak 使用单独的 12 分钟 supervisor 上限。
- 所有常规 fixture 至少连续跑 10 次；涉及竞态的案例按下表次数执行。记录全部失败，不只提交最后一次成功。

## 3. 构建与依赖

| ID | 操作/输入 | 必须观察到的结果 |
|---|---|---|
| B01 | 从干净独立目录按依赖锁构建 | 不依赖作者工作区文件；锁定 NDK/Clang/API/Gradle 等及 FEX/patch/deps |
| B02 | 检查 APK 全部 native ELF、DT_NEEDED 与符号版本 | ARM64/bionic，PT_LOAD 对齐至少 16 KiB；无 glibc loader、GNU libc/libstdc++ 依赖 |
| B03 | 检查 APK ZIP 并关闭 app page-size compat | `zipalign -c -P 16 -v 4` 通过；普通 app 内 dlopen/JNI 初始化成功 |
| B04 | 同一 APK 在 16 KiB 与 4 KiB 运行 | APK SHA-256 相同；app 内检测值分别正确；无编译期伪造页大小 |
| B05 | 单独编译只包含 public API 的消费者 | 不需要 FEX/JNI 私有 include 或其 compile definitions |
| B06 | 关闭新 Android/FEX 目标配置并构建受影响桌面目标 | 不破坏既有桌面构建；缺工具链必须标 NOT_RUN，不能以 lint 代替 |

## 4. CPU、ABI 与 callback

### Foundation 补充验收

以下 F01–F03 在 A16-16K 与 A16-4K 验证；HOST 单独报告，不能代替 Android 运行。

| ID | 操作/输入 | 必须观察到的结果 |
|---|---|---|
| F01 | app 的 DebugBus status/capabilities/stop；与 guest 执行并发调用 | 使用 foundation registry；读取稳定状态，Stop 只投递 owner 请求；不在调用线程改 JIT 状态 |
| F02 | 独立诊断 payload 经 foundation reflection/packing 编码、解码；错误/截断输入 | 字段、schema 与 round-trip 预期一致；非法输入明确失败；依赖是真实 NDK 库，没有同名 stub target |
| F03 | 诊断请求进行中关闭 session/service，重复 100 次 | 停止接收、解绑、等待 in-flight 请求退出再销毁；无悬空 registry/迟到 callback |
| F04 | TCP 连接、无监听、端口占用、超长请求、超时/断线及重启 | **条件 MUST**：启用网络时使用 foundation 网络模块，监听状态真实，退出后 FD/线程清理；未启用则明确记录 capability=false 和 SKIP 原因 |

完整 foundation 的 allocator/TLS/libevent 依赖纳入 B01/B02 与页大小审计。本次准备工作的
API 35 编译和 macOS smoke 均不能将 F01–F03 预填为 PASS。

### CPU / ABI

| ID | 操作/输入 | 必须观察到的结果 |
|---|---|---|
| C01 | x86 整数算术、比较、移位、带进位与分支 fixture | 独立预期 GPR/有效 RFLAGS/RIP/内存正确，不能仅检查退出码 |
| C02 | SSE2 运算、load/store、XMM 保存、MXCSR 舍入控制 | 已声明字段与有效 FP 结果正确，host FP 环境在调用后恢复 |
| C03 | 查询 CPUID/profile；尝试未启用特征 | profile 与 capabilities 一致；不支持特征有定义的 fault/拒绝，不意外终止整个 app |
| C04 | AVX/AVX2 等额外声明特征，含 YMM high | **条件 MUST**：若能力为 true，必须有实际执行/状态恢复测试；若 false，C03 验证拒绝路径并写明 PS4 兼容限制 |
| H01 | HLE：8 整数、9 double、混合参数，分别检查整数/浮点返回 | GPR/XMM/栈溢出 ABI 正确；第 4 参数 RCX、callee-saved、stack/red zone 未破坏 |
| H02 | 两个 owner threads 的不同 FS/GS/TLS sentinel | 线程间不串值；每线程 1000 次 HLE 往返后仍正确 |
| H03 | guest→HLE→guest→HLE→guest，至少两层 callback | 正确返回值、outer/inner 状态、TLS、return gate 与 invocation/depth；100 次往返 |
| H04 | 未注册 syscall/callgate、越界/只读/过期 buffer、未覆盖签名 | 明确错误；无 host syscall 透传、无 host 非法函数调用、内存未意外改变 |
| H05 | native HLE 抛异常、callback 中请求取消 | 转为定义结果；不跨 JIT unwind、不泄漏 invocation，下一次 session 可运行 |
| H06 | 非 owner 发起 InvokeGuest，递归 Run | 调度或明确拒绝，符合 API；不能产生第二个并发执行的相同 ThreadHandle |

## 5. Host page、映射与一致性

| ID | 操作/输入 | 必须观察到的结果 |
|---|---|---|
| M01 | 检测页大小；映射/释放至少 3 个 host pages | 正确对齐；app 内实际 16384/4096；owned mapping 生命周期一致 |
| M02 | 同一 16 KiB host page 放 4 个 4 KiB 逻辑片段 | 独立内容与索引正确，边界写不污染其他片段；仅是逻辑分段，不伪造独立 host 权限 |
| M03 | 申请同一 host page 内冲突的子页权限 | V0 DirectMapped 模式明确 Unsupported；原权限/内容/登记表原子保持不变 |
| M04 | 地址/长度溢出、未对齐固定地址、与已占 host region 冲突 | 明确失败，不覆盖 sentinel host mapping；探测失败保留 errno，EINVAL 不当作 VA 位宽上限 |
| M05 | guard page、未映射/只读写入、跨 host page 普通访问 | 合法访问结果正确；非法访问得到归属正确的 fault，不吞掉越界 |
| M06 | segment helper：相邻 segment 共享 host page，含 BSS | BSS 只清指定范围，前一 segment 数据/权限保持；报告不宣称完整 SELF loader 支持 |
| M07 | host/HLE 将已执行代码的返回常量 A 改 B 并 publication | generation 改变，恢复后仅得到 B；相同地址 100 次修改不出现旧结果 |
| M08 | guest 实际 store 修改代码，再发布 | 实际写入来自 guest；按选择的 TransparentSMC 或 ExplicitPublication 模式失效，恢复后执行新结果；明确记录模式 |
| M09 | 两个 guest threads 运行相同旧 block；控制方 quiesce/patch/恢复 | 事务完成后两线程均只执行新版本；100 个 epoch，不出现 use-after-free 或旧 block 继续执行 |
| M10 | 同一 backing 两个 VA alias，通过其中一个修改代码 | 所有可执行 alias 的译码失效；读写内容一致；不可只按 fault VA 更新 |
| M11 | unmap/remap 同 VA 不同代码，循环 100 次 | 映射世代和译码一致，无陈旧 entrypoint |
| M12 | 一个 host page 同时有 4 KiB code observer 与 GPU dirty observer | 处理写入或移除其中一个 observer 时其他 observer 语义仍成立；保守扩大 dirty 范围可接受，漏通知不可接受 |
| M13 | pin 的 HLE span 与另一线程 unmap/protect 请求竞争 | 等待/Busy 或有定义的事务取消；span 使用期内不失效，释放后可成功 |
| M14 | ARM64 JIT code publication / cache 清空后再执行 | 正确结果与 I-cache 同步；无长期 RWX；ClearCodeCache 不重置 guest 寄存器/内存 |
| M15 | LOCK XADD/CMPXCHG：对齐、未对齐、跨 cache-line、跨 16 KiB 边界的合法地址 | 两 guest threads 各至少 100000 次操作，返回序列/总数正确；无 SIGBUS 丢弃、撕裂或丢更新 |
| M16 | x86 消息发布/锁语义 litmus | fixture 定义的 forbidden outcome 为 0，保存次数与 FEX ordering 配置；不声称因此完全证明 TSO |

M08 若选 ExplicitPublication，必须额外测试“caller 要求 TransparentSMC 时初始化拒绝”。M12 允许使用真实公共 tracker 的 test-double GPU observer，但不得用与 production memory 完全无关的模型替代。M15 的跨页地址必须位于两个已合法映射的 host pages 中；非法跨页 fault 另由 M05 检查。

M16 至少包含 x86 store→store/load→load 的消息发布检查和 LOCK 操作同步检查，fixture 用汇编或明确屏障防止 host 编译器改写预期顺序。不要错误地要求 x86 TSO 下的普通 store-buffering 双零结果永不出现；那会把合法行为当成错误。记录具体 litmus、允许/禁止结果和重复次数。

## 6. 执行控制、状态和调试

| ID | 操作/输入 | 必须观察到的结果 |
|---|---|---|
| T01 | 两 guest owner threads 真正并发执行独立 fixture | native TID 不同、线程进度和 TLS 正确；不把串行调用伪装并发 |
| T02 | 无 HLE、无 yield 的无限 x86 loop 中 Pause/Resume | 热身后 100 次；每次从请求至已退出 JIT/快照发布 ≤1 秒；报告 p50/p95/max，不作为实时性能承诺 |
| T03 | Pause/Cancel 重叠、新请求在 Resume 附近到达 | epoch 不丢请求；fault/取消优先级符合契约；至少 100 次协调交错 |
| T04 | 可取消 WaitingHle 中 Stop | ≤1 秒回到可销毁状态；无长跳转跨锁；后续重启成功 |
| T05 | 停止态写 GPR/RIP/XMM/MXCSR/FS/GS，继续 fixture | 真实执行使用新状态；运行中 setter/stale epoch/stale handle 明确拒绝 |
| T06 | 单步整数、分支（含自跳转）、load/store、SSE2 | 一条 guest 指令的预期效果正确；未污染 guest TF，不能用 JIT block 计步 |
| T07 | Step 遇到 REP、未支持控制指令、HLE gate | 执行前返回声明的 UnsupportedStep/HleBoundary，guest 寄存器和内存不变 |
| D01 | LLDB 停在 native snapshot hook | guest/native 线程对应、寄存器与 fixture sentinel 一致；读取不执行 inferior helper |
| D02 | 从 guest RIP 读取 bytes 并反汇编 | 与 fixture 对应；可用明确工具版本的离线解码补充，不把 host ARM64 disasm 当 guest |
| D03 | 已知 guest fault 或受控 JIT 内 host stop | 保存实际 host context 与归属；guest state 质量明确；Unknown 不得标成精确 fault RIP |
| D04 | ART 活跃/Java 分配与回调期间运行，另测未归属 backend 的 signal | 不截断正常 Java/ART 活动；按已定义的 previous/default handler 行为处理无关信号，不吞信号 |
| D05 | 真实 HLT/非法指令/未登记 return trap | 不返回普通 Returned；有可区分 fault/停止事件 |

LLDB 人为暂停期间不测 T02/T04 延迟。受控错误/预期 crash 用例不能导致整个 suite 被当作成功中止；supervisor 必须逐项记录预期与实际 exit/signal。

## 7. Android、呈现与清理

| ID | 操作/输入 | 必须观察到的结果 |
|---|---|---|
| A01 | 普通 Activity 启动 JNI/FEX；设备无下载 runtime | app 内初始化/fixture 成功；无 glibc/Box64/FEXLoader/X11/Vortek 必需项 |
| A02 | fixture→真实 HLE adapter→host Vulkan 清屏颜色/计数，UI 输入再影响 guest | guest 输出和输入结果可断言；设备 Surface 有对应画面，不只收到 CPU Frame 事件 |
| A03 | 20 次 Surface detach/recreate 或 resize | ANativeWindow 引用和 swapchain 世代正确；无失效 Surface 写入；CPU 可暂停并恢复 |
| A04 | 20 次前后台切换，含 Surface 缺失时 Stop | UI 不阻塞；Stop 不等待 Surface；返回前台状态一致 |
| L01 | 热身后 100 次 create/run/stop/destroy；再重新建立 context | 所有 owned threads、mapping、FD、global refs/ANativeWindow 引用恢复到规定基线；无迟到 callback 写入销毁 session |
| L02 | 连续 10 分钟小型 fixture/绘制/输入循环 | 无 crash/死锁/持续资源增长；记录 CPU/RSS/owned resources，不要求游戏 FPS |
| L03 | 中途失败与测试 timeout 清理 | 已完成用例不丢记录；强制清理记 FAIL/TIMEOUT，不能计为正常 Stop 成功 |

“无资源增长”优先检查模块拥有的对象/映射/FD 计数和重复循环后的平台期；RSS 受 allocator/ART 缓存影响，记录趋势并解释，不要求全进程 RSS 精确回到初值。L01 对自身泄漏的资源计数不允许无限宽松阈值。

## 8. 自动化入口和结果格式

执行者必须实现同等能力的命令行入口，名称可以调整并写入 README：

```text
scripts/android/check-v0-environment
scripts/android/build-v0
scripts/android/verify-v0-artifacts <apk>
scripts/android/run-v0-tests --serial <device> --expected-page-size 16384
scripts/android/collect-v0-evidence --run-id <id>
```

这些命令当前尚不存在。构建、产物检查、安装与测试分离；测试开始时再次核对部署 APK hash/Build ID，防止跑到旧包。fixture assembly 生成必须在构建机完成或使用明确的交叉工具，不能靠设备下载编译器。

每个运行记录至少包含：

```json
{
  "schema_version": 1,
  "spec": "android-fex-v0",
  "baseline_commit": "a712889343ccd2588b712988dae0a76a104e0dce",
  "implementation_commit": "<actual commit>",
  "working_tree_dirty": false,
  "fex_base_commit": "<actual base>",
  "fex_downstream_revision_or_patch_hash": "<actual>",
  "toolchain_lock_sha256": "<actual>",
  "apk_sha256": "<actual>",
  "native_build_ids": {},
  "device": {
    "api": 36,
    "abi": "arm64-v8a",
    "kernel_page_size": 16384,
    "process_page_size": 16384,
    "physical_device": true,
    "soc": "<measured>",
    "kernel": "<measured>",
    "page_size_compat_enabled": false
  },
  "capabilities": {
    "memory_mode": "DirectMapped",
    "smc_mode": "<ExplicitPublication or TransparentSMC>",
    "guest_features": [],
    "step_scope": []
  },
  "cases": [
    {
      "id": "M09",
      "status": "NOT_RUN",
      "iterations": 0,
      "expected": "<fixture assertion>",
      "actual": null,
      "duration_ms": null,
      "fixture_sha256": "<actual>",
      "evidence": []
    }
  ],
  "release_status": "V0_IN_PROGRESS"
}
```

这是 schema 示例，**不提供预填 PASS 数据**。实际运行补充 run_id、timestamps、test runner commit、native libraries/fixture hashes、GPU/driver 信息（显示案例）、拒绝/skip 原因与 logcat/tombstone/截图/LLDB 记录路径。

产物一致性、代码审查与测试结果全部满足后，报告才能使用 V0_ACCEPTED。人工审核最后关注：目标页大小、真实 guest 执行、跨线程停止/失效、bionic 依赖、Step 语义、有效寄存器来源，以及未声称的能力是否确实被拒绝。
