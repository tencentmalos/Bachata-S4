# Android 原生等待、guest mutex 快路径与共享锁审计

本轮先完成生产 mutex 域的并发修复，再按用户追问核对 guest 快路径和其他大锁。
**生产锁仍经 HLE；零 HLE 的 guest 原子锁已在当前真机 FEX 原型中验证，但尚未接入生产 Orbis pthread。**
不改 FMOD 完成条件，不删除堆统计，不宣称 loading 已修好。

## 1. Bionic 不是缺少的那一层

NDK 29 libc++ 的 `__thread/support/pthread.h` 已将 `std::mutex` / `condition_variable`
映射到 `pthread_mutex_*` / `pthread_cond_*`。Android 原生 mutex 无竞争时走原子操作，
竞争时使用 futex；原生 condition 也使用 futex。
参见 [Bionic mutex](https://android.googlesource.com/platform/bionic/%2B/master/libc/bionic/pthread_mutex.cpp)
和 [Bionic condition](https://android.googlesource.com/platform/bionic/%2B/main/libc/bionic/pthread_cond.cpp)。

原实现的主要额外成本来自外围：

```text
guest pthread → FEX 离开 JIT / HLE → 找当前 owner → 检查 guest slot
  → mutex 域全局 guard → 找对象 → VM gate → owner/count 写回 → 返回 guest
```

即使底层用了 Bionic，也不能把这一整条调用当成一次 host pthread lock。
而且 Orbis owner 是逻辑 guest 线程，可能跨物理调用存活，不能简单持有一个
host pthread mutex，然后由不同 host TID 解锁。

## 2. 本轮已经修改的生产路径

`src/core/host_runtime/guest_mutex.h`：

- 每个 mutex 拥有自己的 native guard/condition，稳定对象查询使用原子目录。
  常态 Lock/Unlock/IsOwned 不拿域注册表锁；创建、属性、销毁仍由短注册表锁保护。
- 同步状态保持 guest owner/type/depth 语义。普通 unlock 唤醒一个竞争者，Stop 才广播唤醒。
  stop callback 的注册和销毁不持有它自己需要取得的 guard，避免预取消和回调析构死锁。
- condition 使用对象 guard 和逐 waiter 的 native condition；指定 owner 的 signal 不再唤醒全部线程。
  入队与释放 guest mutex 共同受保护，返回前恢复原递归深度，Stop 保留终止语义。
- guest slot 每次重新 checked read，没有缓存 guest 指针、映射权限或失败结果。
  已销毁 native 对象标记 retired，并随 Session 一起保留到 owners join 后销毁。
  目录8192项对应最多4096次 backing 分配；没有运行中释放后复用的悬空指针。
- 默认诊断 observer 仍为空；启用时不同对象可并发报告，condition 测试记录器相应加锁。

这移除了 **mutex 域全局串行化**，没有消除 VM gate、guest 地址检查或 HLE 往返。
旧版本在确定性反例中会因对象 A 等 VM gate，连对象 B 的 Busy trylock 也无法返回；新版本通过。

## 3. 真机与针对性验证

设备9c2841a4，AYN Thor / API33 / 4KiB / 系统 Qualcomm。
host/JNI 使用 RelWithDebInfo；APK 为 playstoreDebug。

| 检查 | 结果 |
|---|---:|
| 新增 native mutex 并发、取消、定向唤醒、退休对象测试 | 494 / 0 |
| condition，诊断 OFF / ON | 67 / 0；92 / 0 |
| services / rwlock | 73 / 0；60 / 0 |
| macOS LLVM ThreadSanitizer：native / condition | 494 / 0；92 / 0，未报告竞争 |
| 当前 FEX compiled guest 原型 | 13 / 0 |

独立 rwlock 测试 target 原来缺少 native_clock/rdtsc 链接，本轮补齐。
初次 Xcode libc++ 不支持 stop_token 的 TSan 构建失败保留在 build 中；上述成功结果使用 Homebrew LLVM/libc++。
没有运行完整回归。

旧/新/旧同机微基准，游戏已 Stop，每线程2万次 lock/计数/unlock：

| 模式 | 旧 A1 | 新 B | 旧 A2 |
|---|---:|---:|---:|
| 单线程，独立对象 | 23.101 ms | 10.791 ms | 11.079 ms |
| 四线程，独立对象 | 162.747 ms | 171.758 ms | 164.114 ms |
| 单线程，共享对象 | 21.687 ms | 21.837 ms | 21.358 ms |
| 四线程，共享对象 | 742.449 ms | 487.891 ms | 790.852 ms |

共享竞争场景 elapsed 下降约34–38%；独立对象场景没有收益，四线程反而稍慢。
CPU 未绑核/定频，不能把单次最优值当普遍收益，也不能换算成 FPS。

追加原生 host mutex 对照，交替执行5轮、相同线程启动方式与计数工作，中位数：

| 模式 | host std::mutex | 当前 GuestMutexDomain |
|---|---:|---:|
| 单线程，独立对象，2万对 | 3.411 ms | 33.485 ms |
| 单线程，共享对象，2万对 | 3.209 ms | 22.574 ms |
| 四线程，独立对象，8万对 | 11.695 ms | 155.244 ms |
| 四线程，共享对象，8万对 | 11.940 ms | 563.730 ms |

**当前 guest domain 包装确实仍重得多，且这组测试还没有 FEX/HLE 成本。**
测量包含线程调度/启动和缓存竞争，无绑核/频率控制；两个单线程标签实际工作相同，数值差异也表明设备状态波动。
它们量化本次测试的差距，不是严格的每条指令或内核 futex 延迟。

## 4. 纯 guest 快路径已经有实证

当前重新构建 `tests/guest_cpu/compiled_guest/entry.cpp` 的416字节 x86-64 payload，
在真机当前 FEX 运行：

| 同一个 guest 工作负载 | 1万次耗时 | HLE 次数 |
|---|---:|---:|
| guest C++ 原子加锁、计数、RAII 解锁 | 0.400208 ms | 0 |
| 上述工作再额外执行每轮两次 HLE 对照 | 22.244948 ms | 20000 |

双 FEX owner 也正确完成10万次受保护计数；竞争确实进入 host wait/wake，Stop 取消与后续新 owner 执行通过。
这不是完整 pthread 的性能承诺：原型使用固定 backing、简单0/1/2状态字，没有生产属性/递归/销毁/cond 的完整协议。
对照 HLE 也不是实际生产 mutex 的全部工作。不能把两项相除当游戏加速倍数。

合理的生产形态是：

```text
无竞争：guest C/C++ → guest 原子 CAS/acquire → 临界区 → release → guest 返回
有竞争：guest 标记等待状态 → HLE expected-value wait → Bionic/futex 阻塞
解锁：guest release；仅可能存在 waiter 时进入 HLE wake
```

这里“不进 host”指不进入 native HLE；guest 指令仍由 FEX 翻译为 ARM64 执行。
阻塞也完全不进 host，通常只能忙等，或另建 guest 调度器；当前一个 guest owner 对应执行线程的架构不适合那样做。
不应为了零调用把 FMOD/FIOS 的等待改为持续自旋。

生产接入的关键不是再写一个 CAS，而是统一状态和生命周期：

1. 使用同一个 guest-owned 原子状态作为权威状态，host slow path 只管理等待队列/取消。
   不能保留当前 host `Mutex::owner/depth` 为第二套权威值，否则 guest CAS 和 CondWait/Destroy 会分叉。
2. 当前 owner 已放在 guest `Tcb::tcb_thread`；x86-64 FS base 指向 TCB，布局偏移0x10。
   生产应从 guest TLS 直接取得该 handle，与当前 HLE `Current()->handle_va` 对齐，不能每次锁再调用 pthread_self HLE。
   其他 guest CPU 需使用自己的 TLS/ABI 适配，不能照搬 FS 偏移。
3. 覆盖 pthread 与 scePthread 两套入口及 IsOwned/trylock/递归/errorcheck。
   guest libc 会修改对象+0x20 flags，原有可见 ABI 前缀须保留。
4. CondWait 的入队、释放、signal/broadcast、递归深度恢复和 expected-value wait 必须共用新协议。
   判断后再睡眠不能留丢唤醒窗口；取消不能导致剩余 waiter 永久睡眠。
5. 销毁、Session generation、映射身份与 VM publication 一并处理。不能把裸 guest VA 永久交给 futex，
   也不能在慢等待期间保留阻止 VM drain 的 writable pin。
   受控 backing 生命周期与 wait-entry 的失效/唤醒要有显式协议。
6. 作为跨游戏 libkernel/pthread guest 实现接入 resolver/custom SDK；游戏相关 patch 只做精确导入/代码身份绑定。
   不能只替换 TMNT 某个 malloc 包装函数来冒充通用锁实现，也不能混用旧 host owner 和新 guest owner。

**本轮未启用上述生产 guest 快路径。** 当前安装版本是第2节的 host domain 修复。

## 5. 其他“GIL 类”共享锁：已核对的范围

| 位置 | 实际覆盖 | 结论 |
|---|---|---|
| `GuestMutexDomain` | 旧域锁覆盖全部锁对象及 VM 写回；新 Lock/Unlock 使用对象锁 | 本轮已移除这一层全域串行化 |
| `GuestRuntime::vm_mutex` + `GuestAddressSpace::lock` | checked read/write、owner/count 发布、映射事务；Read/Write 在 space 锁内 memcpy | 仍为共享串行点；快路径应绕开每次 HLE 检查，但不能删除映射安全协议 |
| `guest_runtime.cpp` 的 StorageEntries handler | 非 save 的**整个 DispatchStorage** 持有 `vm_mutex`，包含读盘和 fsync | 确认是过宽临界区：磁盘调用会挡住别的 mutex 写回、clock 输出和 VM 操作，优先处理 |
| `GuestStorage::mutex` | 文件表及 read/preadv/write/fsync、目录和 quota 扫描 | 第二层 I/O 全域串行化；不同文件也互斥。本轮未修改 |
| `GuestRuntime::Current` | 每次取 owner 获取 threads_mutex、查表并复制 shared_ptr | 高频短锁，未跨 guest 执行持有；可用已有 owner 作用域/TLS 减少热路径查表 |
| FEX context lock | HLE 前的 owner/snapshot/lease 状态迁移及返回后恢复 | 有共享临界区，但 `ExecuteThread` 和 native adapter 调用不在整个锁内，不是全程执行 GIL |
| HLE registry | Find 使用 shared_lock 并返回 shared_ptr；随后解锁调用 adapter | 读者可并发；仍有查询/引用计数成本，无整调用全局锁 |
| rwlock、POSIX/kernel semaphore 域 | 仍有域级 mutex/共享 condition；睡眠时释放域锁 | 可能放大短操作竞争或无关唤醒，不能说睡眠期间锁住全部 guest；需按实际频率继续排序 |

I/O 不能只把外层 `vm_mutex` 删掉：它目前还保护一组 pin 的整体准入，防止 VM coordinator 在获取多个 pin 中途开始 drain。
需要把准入与慢操作分开：先在短 gate 内准备完整 pin/descriptor lease，然后释放 gate 执行 I/O，
最后释放 pin；完成阶段不得在保留 pin 时反过来等待 VM gate。
文件状态应按 descriptor/open-file 对象持有生命周期和顺序锁，save quota 单独保持跨文件正确性。
Close、Unmount、O_APPEND 临时切换、取消和同 VA remap 都需要针对性反例，不能只追求缩小 lock_guard 代码范围。

上述是源码确认的串行化边界，不代表已量出每把锁的实际阻塞占比。
当前 loading 尚不能归因于某一把锁，也不能把 FIOS 等待时间等同磁盘用时。

## 6. 普通 APK 观察和限制

安装 APK `00d980fc89286255e5ceb6a83867fb76ba84ef248eeb5eef69c7b8c81aea462d`，
host `bb6ca1a6f631249a0a35c9d408a5bc36941e599e5b46009033a9ce39c5331bf6`，
JNI `c173ed47d7f610b6837af4f8dc565162e7fa63b5943e5cfd5c755159bd8fdb86`。
与旧 APK 相比包内变更项是 host/JNI 两个 DSO；JNI 重新链接，不能声称仅 host 二进制不同。

旧 A：PID30873/gen1，UUID ad2420bc9e11a1983741904c58030ca0。
新 B：PID5965/gen1，UUID 4f07eef732fe60115075f31c48a5f9de。
二者均用同样的 Cross warmup，120s 文件记录，`profile_sync=0`；都进入屋顶 MOVE，真实 UI Stop 达到 Stopped/user_stop。
自动 warmup 结果仍是 TIMEOUT_UNVERIFIED；截图确认到达屋顶不等于自动测试通过、操作验收或十分钟稳定性。

结束时已关闭启动文件采集属性与额外同步诊断，并用同一新 APK 重开至 PID5965/gen2。
最终截图为屋顶 MOVE、约12 FPS；自动输入结束，TracerPid0，未挂 debugger。
[恢复状态](native-locks-20260917/restored-status.txt)与[最终画面](native-locks-20260917/restored-final.png)独立归档。

| 整个120s记录 | 旧 A | 新 B |
|---|---:|---:|
| Session 到真正首次 present | 19.436 s | 17.919 s |
| 主 owner scePthreadMutexLock 次数 | 1,657,489 | 1,669,229 |
| 上述 Lock elapsed | 6.844 s | 4.955 s |
| 对应 Unlock elapsed | 5.400 s | 3.423 s |
| 主 owner condition wait elapsed | 13.866 s | 13.518 s |
| 主 owner usleep elapsed | 28.336 s | 28.982 s |
| 呈现计数最后值 | 936 | 1065 |

这里是整个记录的描述性数据，**不是同一 loading 业务区间的严格 A/B，也不是 on-CPU 时间**。
输入时刻、阶段占比、热状态不完全相同；A 首分钟还与一次约0.5s的 CLI 测试重叠。
因此只能说生产锁包装的观测 elapsed 下降，而等待过程仍在；不能从总帧数宣称游戏加速。
两份文件完整、0 skipped chunks、已选 scope 配对0 unmatched；末尾各54个未闭合 scope 保留，不宣称全局无损。
本轮 GPU query 关闭，不能据此推断 GPU 耗时。

在安装新 APK **之前**，旧 APK 的同进程重开曾发生 Guest-1 SIGSEGV/SEGV_ACCERR
（PID14213/gen3、tid28702、地址0xa655f8930）。证据保留，未归因为新锁，也未声称历史崩溃消失。

## 7. 证据和复现

- [manifest / 源码与产物 SHA](native-locks-20260917/manifest.json)
- [原生锁对照](native-locks-20260917/host-compare.txt)、[真实 FEX guest 原型](native-locks-20260917/compiled-guest.txt)
- [A 分析](native-locks-20260917/a-analysis.json)、[B 分析](native-locks-20260917/b-analysis.json)
- [B 屋顶截图](native-locks-20260917/b-rooftop.png)、[B 正常停止](native-locks-20260917/b-stopped.txt)

原始 PROF、完整构建日志、旧 header、未安装的中间实现测试留在 `build/native-locks-20260917/`。
小型 raw 日志、分析器和 hashes 归档到本报告同名目录；大体积 PROF 不复制进 docs。

```sh
cmake --build build/graphics-toolkit-review/clock-native \
  --target guest_native_mutex_tests guest_condition_tests compiled_guest_tests -j 6
# push 对应产物至设备已有 libc++_shared.so 的目录后：
LD_LIBRARY_PATH=. ./guest_native_mutex_tests
LD_LIBRARY_PATH=. ./guest_native_mutex_tests --host-compare
LD_LIBRARY_PATH=. ./compiled_guest_tests
```

保留原有 dirty work；未改 FEX 子仓、Foundation、驱动、音频 bridge、FMOD/heap 查询语义；没有 commit/push 或新执行 spec。
