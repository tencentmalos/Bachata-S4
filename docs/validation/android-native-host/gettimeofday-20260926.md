# Android/FEX gettimeofday 热路径优化（2026-09-26）

状态：实现、Android 构建及 AYN 独立 native/生产 FEX 测试完成；已整理为本轮 gettimeofday 源码与验证提交。新 APK 已构建并保留，未覆盖正在运行的血源，尚无新包游戏性能验收。GCN 反汇编工作按用户要求取消；PSO 工作是独立的[设计方案](../../specs/pipeline-compile-cache-20260926.md)，未实现。

## 动机与改动

[血源完整启动采集](bloodborne-startup-upload-20260926.md)中主 Guest 有 18,267,329 次 `gettimeofday`、HLE scope 累计 elapsed 6.704371 s，主要集中菜单；这不是完整 Guest→Host 往返成本，也不是已证明的加载唯一瓶颈。原路径为真实 Host HLE `GuestClock::Read(CLOCK_REALTIME)`，本次仍保持这个路径。

- `src/core/host_runtime/guest_gettimeofday.h`：常见的 timeval-only 调用直接使用 `GuestAddressSpace::WriteData`，删除之前重复的 `ValidateRange` 查找/加锁。WriteData 自身持锁验证准入并 pin，复制后仍保留原有 Guest 写通知与 lease 释放；没有缓存裸 Guest 指针或绕过映射/权限检查。
- `guest_runtime.cpp`：两个 NID 直接读取 ABI 实际使用的 RDI/RSI；去掉通用六参数数组及额外 type-erased 包装。POSIX 的 errno/-1 与 SCE `0x80020000 | errno` 编码保持，SCE 只接收 timeval。
- 时间仍每次读取 `CLOCK_REALTIME` 并转微秒，不缓存时间、不用单调时钟或 RDTSC 近似；支持主机时钟校正语义。
- 可选 timezone 路径继续先验证两个输出再写，UTC timezone 行为不变。两个输出仍是分别 pin/write，未增加并发 remap 下的整批原子语义；WriteData 拒绝时显式返回 EFAULT，不把失败写回当成功。

## 功能与性能证据

AYN serial `9c2841a4`。测试在 `/data/local/tmp/shadps4-gettimeofday-20260926` 的自有目录中运行，未使用游戏存档目录，也未改驱动、频率、应用设置。

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| Android host + `guest_gettimeofday_tests` 构建 | 通过 | NDK 29 / API 33 / arm64 |
| Native 功能 | 40 checks / 0 failures | 对齐/非对齐/映射尾、实时钟夹取与微秒范围、null/双输出/UTC、坏地址/溢出/洞/只读/跨独立映射、只写权限、通知与无 pin 泄漏 |
| 生产 Module/Linker/FEX fixture | 旧/新各 18 轮，全部 Returned `0xcafe`，进程正常退出 | 每轮 500,000 次实际 Guest import 调用加 ABI 错误路径；旧/新各 900 万循环调用；未模拟游戏负载 |
| Native body 六组配对 benchmark | 均值 184.36 → 162.82 ns/call（−11.7%） | 同一二进制包含旧 Validate+Write 对照和新 helper；不含 FEX 边界；最后一组波动保留 |
| 固定 CPU7 的真实 FEX benchmark | 每轮进程 CPU 均值 390.588 → 369.569 ms（−5.38%） | 旧/新各 12 轮，每轮 500k 次，测 `backend.Run`（含启动/检查），不是函数 exclusive 时间或 FPS |
| 新 APK | Gradle assemblePlaystoreDebug 成功；merged/stripped/APK host 逐字节身份核对 | 未安装到游戏进程 |

Native benchmark 前五组基本稳定在 183/160 ns，最后一组 191/177 ns；总均值包含全部六组，没有删掉较差结果。真实 FEX 测试分两组：4 个未固定 CPU 的进程（每个 3 轮）用于 ABI/退出检查；8 个 CPU7 进程（旧/新交替，每个 3 轮）用于上述统计。未固定 CPU 的结果存在重叠与漂移，不声称稳定改善。

固定 CPU 不等于锁频。测试期间血源 PID9560 仍运行，系统温控/cpuset/调度可能变化；5.38% 是这份合成测试的观测均值，不能外推成游戏帧率或启动时间收益，也未给出统计显著性结论。扩展尾轮 `fex-pinned-8-after.log` 在 `taskset` 设置 affinity 时返回 Invalid argument，程序尚未启动、无 metrics；明确剔除，未尝试强行上线 CPU/改 cpuset。没有将它算作 Guest/FEX 失败。

原始简表：[benchmark-summary.json](gettimeofday-20260926/benchmark-summary.json)，[native 功能/benchmark](gettimeofday-20260926/device-unit-bench.log)。逐进程日志保存在同目录；完整构建、fixture ELF、设备导入日志及旧库位于本地 `build/validation/gettimeofday-20260926/`。

## 测试 runner 退出问题

首次旧、新 host 都完成三轮后，在静态退出时因 logging worker 仍存活触发 destroyed mutex/FORTIFY，exit134；这些不作为干净通过记录，原日志 `fex-0-before.log` / `fex-first-after.log` 保留。`production_runtime_runner.cpp` 原本 Setup/Flush 日志但未 Shutdown，本次加局部 RAII 在退出前调用 `Common::Log::Shutdown()`。修正后旧、新库各 18 轮/6 个进程均 exit0。

runner 另增加可选 `SHADPS4_RUNTIME_METRICS`，默认没有性能输出；开启才输出 elapsed/process CPU。上述退出修复属于测试工具生命周期，不能解释为 gettimeofday 或游戏崩溃修复。设备生成的 `fex-fault-*.txt` 文件本轮为空，不能仅凭文件名认定发生 Guest fault。

## 版本身份和收尾

工作树基线 `b4972cbdf8231eed2111b21b71661f4194bf54cb`，保留已有其它 dirty 修改。测试 runner 链接本轮 `build/android-host-api33/native/shadps4-host-loader.cmake`，未使用旧 SBS 构建的 host 配置。

| 项目 | SHA-256 |
| --- | --- |
| 旧 host | `cffae94c2bb931302e3573dfdf3038ccb612405ee9081ea48fbdd4e92d465639` |
| 新 host / APK 内 host | `178a207f1ab4d258598fbbd679c8721cde4083e3dc987cddf2853d962c37e235` |
| 新 APK | `2fcc46498053940a7efe942cd29435870ebdd456401b93176d9b4865ab8136a6` |
| APK 内 FEX JNI | `7ea201f9600b13f673b9689ae5d1be80f20e778a53018ef6e8e0b099ae2d2396` |
| 合成 ELF | `f118a6f7b72ebf7fd88758bd667f9662c0176eb6a0e133c429508f5fdcb94786` |

完整[测试身份与清理清单](gettimeofday-20260926/device-test.json)。APK 固定保留在本地 `build/validation/gettimeofday-20260926/app-gettimeofday-debug.apk`，构建输出也仍在 Android app 目录。

结束前确认无本轮 standalone 测试进程，拉回自有测试日志，删除且复核上述唯一自有设备临时目录已不存在。应用 PID 仍为 9560，未安装/重启应用，未主动操作游戏输入/设置/存档；不能以此声称用户运行中的游戏存档字节不变。没有 debugger、forward、新 profiler 采集或锁频操作。本轮清理不代表其它历史任务的 CleanupPending 已全部解决。
