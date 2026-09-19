# CPU 地址查询 / HLE 边界优化与 GPU timestamp 核查

2026-09-15。本轮按用户要求先优化 CPU 高频路径。保留既有 dirty 工作，不改 FEX 子仓、Foundation、Oboe、游戏文件或 GPU 渲染行为。只进行变更相关检查和一个实际教程场景的性能复验；没有完整回归或新 spec。

## 实现

1. `GuestAddressSpace::Query / ValidateRangeLocked` 增加 64 项直接映射的索引提示。缓存的是 `mappings` 的下标，**不缓存指针、权限、generation 或失败结果**。每次命中都在原 address-space mutex 下重新检查当前记录是否完整覆盖请求，再读取当前权限；下标越界或不匹配回退原线性查找。不重排映射，不移除校验，不跨不同 mapping 拼接范围。erase/split/remap 即使改变了下标含义，也不能复用旧权限或旧指针。未增加无调试器状态下的采样/计数。
2. `GuestMutexDomain` 将 owner/count 的两次 VM gate/校验/写回合为一个 12 字节 checked write，保留 spin/yield/protocol/flags。Host map 仍是锁所有权的权威状态；竞争等待、条件释放/重获、recursive/errorcheck/trylock、取消及 VM gate 规则不变。这也避免第一次写回成功、第二次失败时留下半更新状态。
3. FEX adapter 的 syscall frame registry 改为一次物理 `Run` 登记/退出时 RAII 撤销，不再在每次 HLE continuation 上反复分配/删除 map 节点。嵌套 InvokeGuest 在相同 frame 上用引用计数保留外层登记。源码依据是 FEX `InternalThreadState::CurrentFrame` 为 const、指向自身 BaseFrameState。正常 owned dispatch 直接使用当前 Run 持有的 fault flag；frame 不匹配/没有 binding 仍走弱引用表兜底。没有减少寄存器重建、safe-point snapshot、FP/TLS 切换、epoch/取消或 debug 边界正确性要求。

**本轮没有把编译 guest mutex 原型启用为生产绑定。** 原型继续通过真实 FEX 检查，但它只有单一 atomic word；生产 mutex 还包括 host owner/depth、guest ABI prefix、static init、类型/条件变量、映射退役等协议。直接插 CAS 会形成两个互不知情的所有权状态。本轮已优化现有生产路径，后续迁移需要一次性统一这些对象协议，不能把原型 13 项通过写成生产 pthread fast path 已接通。

## 定向验证

设备仅 AYN Thor `9c2841a4`，ARM64/API33/4 KiB，未操作 Swan。

| 检查 | 结果 |
| --- | ---: |
| guest CPU address-space contract | 47/47 |
| mutex / services | 73/0 |
| condition / cancellation | 66/0 |
| 真实 FEX HLE / 两层回调 / FP / 坏指针 / 故障归属隔离 | 43/0 |
| 真实 FEX publication / continuation / Cancel | 6/0 |
| guest debugger / mixed stack / watch / detached path | 85/0 |
| 编译 C/C++ guest 原型、竞争 wait/wake、Cancel | 13/0 |

扩充的 M33 验证热查询后删除前序 mapping、权限收紧、same-VA remap、洞及 mapping generation，不允许查询改变外部可见的映射顺序。mutex 增加 owner/count 相邻 ABI 字段 canary。新增 `--focused-hle` 仅组合已有相关测试，未知 selector 仍非零退出。

新增 `guest_vm_mutex_bench`：同样的非单调 2056 个映射、8 个热点 slot，200000 次 ValidateRange 和 20000 组**直接调用生产 host domain**的 lock/unlock。它不经过 FEX，也不是游戏 FPS 基准。设备三轮中位数：

| 操作 | 旧实现 A | 优化 B | 重新运行旧二进制 A |
| --- | ---: | ---: | ---: |
| 200000 次查询 | 399.44 ms | 7.02 ms | 412.71 ms |
| 20000 组 mutex | 251.51 ms | 7.88 ms | 255.86 ms |

这是构造的高 locality / 大 mapping 表工作量，实际效果取决于缓存碰撞、访问分布和 mapping 数量；不用于承诺 32×/57×游戏提速。设备仍运行游戏，未锁 CPU 频率/温度，保留所有轮次而不只挑最快值。FEX 原型本轮 10000 次纯 guest lock/unlock 0.281 ms、加入20000次空 HLE 为22.447 ms；没有同一时段的旧 FEX 对照，不能把与历史数值的差异单独归因 registry 优化。

## GPU time query 实际状态

结论：**当前生产 Android 没有接入可用的 GPU timestamp query 结果。**

- `src/common/debug.h` 直接定义 `TRACY_GPU_ENABLED 0`，不是只在 Android 缺少扩展。当前桌面源码虽保留 Tracy Vulkan 路径，默认同样被此宏关闭；不能声称当前 desktop GPU 计时已在运行。
- `vk_instance.cpp` 的 calibrated timestamps 扩展/Tracy context、`vk_scheduler.cpp` 的 guest GPU scopes/collect、`vk_presenter.cpp` 的 Host frame zone 均依赖该路径；实际 Android cache 的 `TRACY_ENABLE=OFF`。
- `Common::Profiler` / Foundation ring 桥当前输出 CPU scope/counter/bookmark/frame；shadPS4 侧没有接 query pool、`writeTimestamp`、availability 异步读取和 `timestampPeriod` 换算结果到这个桥。
- StatusLayer 明确显示 `GPU time unavailable | Host frame intervals`。先前 66.5 ms `VideoOut.Prepare` 是 host wall span，不能改名为 GPU time。

后续若接 GPU query，应单独实现有界 query pool/分批退役、非阻塞 availability 读取、valid bits wrap 与 period 换算，并按 Session generation、draw/present queue 和提交区间标注；未就绪/无能力显示 unavailable。UI 重绘/重复 present 与新 guest frame 分开计时，不能通过等待 GPU 来取一个会反过来降低帧率的值。本轮按“确认是否接入”的要求做核查，没有启用整个 Tracy 或顺带改动 Vulkan 队列。

## 实际 APK

新 APK SHA-256 `b4c92212d8aa1ce07c60eb5bf8cf489edb798a0d0f1758ba78b54acc5dc18e8c`；host Build ID `641421e85e780093f005b8a15a23e5f9ffbe621d`；JNI Build ID `4e5c8288145dc85b4f8ea2a44dfd63eed49329a9`。host/JNI RelWithDebInfo、FEXCore Release，原 Turnip。设备安装包哈希匹配。

本次 PID25383 / generation1 / run_uuid `f88b351e80ff7e34439f4e0272f1ab71`。实际 warmup 已进入屋顶、左摇杆推动角色/镜头、MOVE→ATTACK，并收到 **GAMEPLAY_REVIEWED / exit0**。未挂 debugger，停止自动输入后留在相同教程区域采样；没有用片头/菜单代替游戏场景。

同样 `cpu-clock:u / 100 Hz / frame-pointer / 45秒` 的用户态 simpleperf：**5747 samples / lost0**。与前一轮旧 APK 的6393样本对照：

| 实際关卡用户态样本 | 旧 APK | 新 APK |
| --- | ---: | ---: |
| ValidateRangeLocked / FindContainingMappingLocked 叶节点合计 | 789 / **12.34%** | 163 / **2.84%** |
| 任一 GuestAddressSpace 方法在调用栈中 | 1379 / **21.57%** | 536 / **9.33%** |
| VM 方法作为叶节点 | 1005 / 15.72% | 256 / 4.45% |
| 样本 period 累计 | 63.93 CPU秒 | 57.47 CPU秒 |

新增 helper 一并统计，避免仅因函数拆分就宣称原函数消失。总 period 约少10.1%，但设备未锁 CPU频率/温度、不是逐像素相同状态，不作为受控全游戏提速保证。旧基线和新 capture 均为实际 ATTACK 教程，截图/精确二进制/分代身份独立保存，不拼接同一运行。

46.64秒计数窗口中新 guest flip 增579，均值 **12.415 FPS**。35秒 PROF：433个完整 `VideoOut.Prepare` 区段，均值 **66.624 ms**，与旧66.506 ms接近。20次设备GPU busy均值85.61%，频率615/680 MHz，旧采样全为680 MHz；仍不能声称FPS提升或精确GPU执行时间。画面和实际输入保留，未再出现此前地址排序尝试的纯色回退。

PROF 本次34.984秒捕获四类mutex lock/unlock合计3,097,533次，约 **8.85万次/秒**，高于旧约5.75万次/秒。优化减少了单次校验/登记/写回成本，**没有减少生产 HLE 次数**；相同同步/轮询循环更快可能增加调用频率，这是推断，尚未用guest栈证明具体循环。不能把调用增长写成游戏有效工作增长，compiled guest协议迁移仍有价值。

本次 PROF 的 chunks_skipped=0；沿用保留 per-thread wire order、丢弃跨缺口未闭合 spans 的分析脚本，排除捕获边缘不完整区段。CPU样本与trace的分析程序均在证据中保留。游戏保持运行、自动输入停止、ring恢复ON、stream capture结束、GPU Reshape OFF；未发生上一轮LLDB attach扰动。

证据：[artifact清单](cpu-boundary-opt-20260915/artifacts.json)。大体积APK/PROF/perf.data保留于清单指向的本地build目录；提交候选文档目录仅存报告、摘要、定向日志、源diff及两张关卡图。没有 commit/push。
