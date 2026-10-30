# KH3 长会话退化：同步窗口按需扩展与地址查询索引（2026-10-24）

## 结论与验收边界

已修复[现场诊断](kh3-lowfps-audio-20261024.md)确认的两个规模退化路径：同步对象超过
首个 256 MiB 窗口后丢失 guest 快路径，以及地址提示未命中后线性扫描全部映射。
适用于所有使用该运行时的游戏，没有 KH3、音频线程或游戏地址特判。

Odin3 的生产同步域/FEX payload 回归、地址权限/退休回归通过。APK `607e9761`
已按用户保存后的授权覆盖安装并核 SHA，新的 PID 31742 已进入世界地图。
短测的帧周期与音频供给正常，用户随后切后台，保留暂停状态。
这些结果证明修复路径和查询开销，不能替代实际游戏的帧率、爆音或长时间游玩验收。

## 实现

### 同步窗口按需扩展

- 保留原先两块 256 MiB 初始窗口与 guest 的快速范围检查，mutex/condition 和 rwlock
  仍分属不同区域。窗口不足时分配新区域，通过独立 RW 目录发布，不修改运行中的 RX 代码。
- 每个扩展区域首个 16 KiB 块保存不可变的 `{next, base, limit}`；对象区域排除这个块。
  宿主在返回对象地址前 release 发布头指针，guest acquire 读取后遍历范围。
  mutex/rwlock 使用独立链头，避免跨类型对象被当成有效锁。
- 优先申请 256 MiB；ENOMEM 时按 16 KiB 对齐减半，最低 32 KiB，适应碎片化地址空间。
  全部失败不发布半成品；后续申请仍能重试。没有固定窗口数或累计对象数上限。
- 地址和范围节点保留到会话结束，不复用已销毁对象。新增 mutex tombstone，host/guest
  争用路径均以 CAS 更新状态；复制出来的旧句柄不能重新取得或复活已销毁的锁。
- 内存仍随会话内累计对象数增长，受实际可用虚拟地址/内存限制；本次没有引入不安全的
  对象回收或承诺无限内存。初始窗口分配失败时也可按需申请较小窗口。

实现入口：`guest_sync_arena.h`、`guest_sync_abi.h`、`guest_runtime.cpp`、
`guest_mutex.h` 和 `guest/runtime/sync/mutex.c`。协议见
[`guest/runtime/sync/README.md`](../../../guest/runtime/sync/README.md)。

### 映射查询

`GuestAddressSpace` 保留原有公开 ledger 顺序和 64 槽提示；新增按基址排序的索引，
只存基址和 ledger 下标。提示未命中后用二分查找，仍从真实 ledger 校验范围、权限。
Map、MapFreshData、Unmap、局部替换和 token 下的 VM 修改均同步维护索引；所有所需
容量在 VM syscall 前准备，失败保留旧状态。没有复制权限或长期缓存 Mapping 指针。

## Odin3 回归

设备 `6edb0593`，独立 `/data/local/tmp/kh3-adaptive-sync-20261024` 测试进程；
没有退出、恢复或向运行中的游戏注入输入，也没有修改游戏配置/驱动。
最终源码测试结果：

| 测试 | 结果 | 关键覆盖 |
|---|---:|---|
| guest_sync_fastpath_tests | 97 / 0 | 小窗口强制扩容、新旧窗口零 HLE、在另一 FEX owner 执行时发布扩展、类型/对齐/元数据/空洞拒绝、旧句柄 |
| guest_native_mutex_tests | 65,507 / 0 | 原有锁生命周期及并发；110 万次唯一地址分配跨真实 256 MiB 边界；碎片化缩小申请、OOM 后重试 |
| guest_cpu_contract_tests | 51 / 0 | 2048 个乱序映射、256 轮混合增删/权限变更/失败回滚；原有退休、pin、公共顺序和 stale hint 检查 |
| guest_condition_tests | 67 / 0 | 等待、释放、取消、回收等条件变量语义 |
| guest_services_tests | 82 / 0 | 时钟/暂停等共用运行时回归 |

110 万次是 **arena 地址分配**，不是 110 万次真实 mutex 创建/销毁；生产域原有
2 万次锁生命周期检查也仍运行。FEX 小窗口测试强制首窗 16 KiB，扩展 64 KiB 请求
只能按 32 KiB 成功，包含非连续地址和真实并发发布。

负对照仅关闭 guest 对扩展范围的识别，保留新 ABI 与其余代码：97 项中准确失败
SF01（扩容后零 HLE）和 SF02（新发布期间旧扩展窗口仍为零 HLE），其余通过。
这证明新测试能抓住旧的扩容后退回 HLE 行为。

Mac 另用 Homebrew LLVM/libc++ 运行同一地址空间测试并通过；M13f 因宿主拒绝
RWX 而显式跳过，Android 设备上已覆盖。系统 Apple libc++ 不提供此构建需要的
`std::stop_token`，首次编译失败日志也已保留，没有为此修改项目源码。

设备独立测试目录已删除，测试二进制 SHA 与日志保存在本地证据目录。

## 查询开销 A/B/B/A

同机固定 CPU 6（`taskset 40`），没有锁频或修改 governor。A 为 HEAD 中的旧地址查询
实现，B 为本次索引；其余同一 benchmark、NDK 和编译选项。每组运行 3 轮，表中
分别汇总两次 A / 两次 B 的 6 个样本中位数。每轮随机分布 20 万次范围/权限检查，
映射以乱序创建且中间留空洞，建图时间不计入查询。

| 映射数 | 旧线性扫描 | 新索引 | 查询耗时下降 |
|---:|---:|---:|---:|
| 2,048 | 145.772 ms | 6.346 ms | 95.65% |
| 8,192 | 577.047 ms | 9.172 ms | 98.41% |
| 16,384 | 994.990 ms | 11.682 ms | 98.83% |

这是地址查询的微基准，不能按比例换算游戏 FPS。原始计划 32,768 映射加空洞时，
独立测试进程触及 Android `vm.max_map_count=65530` 而失败；改用 16,384，保留失败
证据，不计为通过，也未改变设备 VM 限制。

## 构建与身份

- 分支 `feature/malos/swan_performance`，HEAD `22bd5a9d76fa32b726cf14831a00373795e200ac`
  加当前工作区修复；未提交。保留之前音频、会话和慢帧采集修复，不含已撤回的管线候选。
- Android host、最终独立测试和 Gradle `:app:assembleDebug` 均通过。
- APK SHA256：`607e97614cf556ca157dd21f2b3b3ee8dfa4b7ed3c0f2a30c28ce5e580652143`。
- APK 内 host SHA256 与本地 SDK 一致：
  `3bda13e719386596e464a0804268ff1639e24f54e354c73225a1005b23773c95`；
  Build ID `2d41576c0811a9a0318e7ddc8716d026523c0a93`。
- 保持设备现用 mainline Turnip `68dafb89a26` / SHA256
  `d7e6746a5eb02500f8d4ac3022ae8459a3e683b56ba55b6dc2705b5706f46099`，已核 APK 内字节。
  最初重建驱动因 Python 缺 Mako 失败，随后通过受支持的 `SHADPS4_TURNIP_PREBUILT`
  绑定相同源码身份及原有驱动；没有更换驱动或调整设备属性。

本地证据：`build/validation/kh3-adaptive-sync-20261024/`，包含测试及负对照日志、
微基准 A/B/B/A、候选 APK、native/Gradle 构建日志、身份与测试二进制 SHA。

## Odin3 安装与短测（13:57–14:03）

用户明确“已保存，可以安装”后，13:57:35 覆盖安装成功，设备实际 `base.apk`
SHA 与候选 `607e9761…` 完全一致；没有清数据、存档、配置或 shader cache。
通过 `game_id=CUSA15072` 启动，新 PID `31742` / generation 1 /
run UUID `1ca99f4f9fa5fdba3d390585a90ec8c3`。运行中的驱动仍为 `d7e6746a…`。

- `hle_sync status` 为 `fast_path=installed`；生产 payload SHA
  `3d48e60611e31a5fb0f914a85b65a4136916ceb286e66cd88d18666f53f0b533`。
- 预载 3009/3009 管线，driver cache 69,613,950 B 正常读取。后续场景产生新管线，
  所以没有把新 shader 计数必须为零当成缓存生效条件。
- 13:58:52 截图为世界地图（星光大道/黄昏镇），叠加层约 60 FPS。没有发送手柄、
  触摸或角色移动输入；进入后的操作由用户控制，截图不冒充其他战斗场景。
- PCM 请求 10 秒，但 13:59:30.986 用户切后台，实际只有约 8.6 秒在运行。
  主音频端口 0x20000004 共 1610 块，即 8.5867 秒音频，首末块跨度 8.5981 秒；
  P95 间隔 5.789 ms、P99 6.202 ms、最大 13.818 ms，>10 ms 3 次、>20 ms 0。
  共 4831 块、0 dropped。不能把暂停后的空白当成缺音，更不能称为完整 10 秒采样。
- 主端口左右与四个环绕通道均有信号，peak≤0.837、无 NaN/Inf；中心/LFE、视频端口
  和手柄扬声器在这段内容中为零，不由此推断声道故障。供给与有效运行时间接近，
  没重现此前约 32% 的缺口；没有做扬声器主观听感或长期爆音验收。
- 音频设备 `reconnects/stalls/error=0`。日志记录 14:00:03.970 同 PID 从后台恢复，
  14:00:18.055 再切后台；这仅覆盖约 33 秒的暂停恢复，不是过夜测试。
- KGSL faultcount 仍为 15，未新增 GPU 故障；日志未出现窗口扩展，短会话尚未达到
  第一窗容量。真实扩容仍只有前述压力/FEX 回归证据，长游戏会话待继续验证。

### 帧与 HLE 记录

PROF `6edb0593-31742-ring-1792821588291699-1.prof`：44,761,244 B，SHA
`8f9bdc5f40717bc8097b58b1ce50d61541c5fd9c7f31e3de5e058a999430e20f`。
Litep index `37a03ab222a44529b94bfce88c6d9726`：2,010,013 events，171 chunks，
0 skipped、0 pairing errors、未截断；边界未闭合 scope / flow 缺来源仍保留。

在音频采样内选取 owner 连续的 SDK 4607–4957（351 周期，5.856 s）：平均
16.684 ms、中位 16.672 ms、P95 19.622 ms、最大 34.106 ms，>33.333 ms 1 次。
它们是 GNM 提交周期，不是独立显示时间戳。后续多个提交线程交替，8.3 秒完整范围中
74 个 `frame_owner_changed` 被排除，不能把筛出的均值当成整段总帧率。
这与旧故障场景/运行时长不同，不是性能 A/B 或长会话修复率。

同一 8.3 秒范围内，Guest-51/52 的 ring 记录覆盖完整：无 mutex Lock/Unlock HLE，
有 4+18 次 mutex Destroy 及少量 shadSyncWait/Wake；不是关闭了采样。AJM job run
2482 次、batch start/wait 各 1574 次、AudioOutOutputs 1560 次均保留。
GpuComm 只留约最后 3.2 秒，未用它推断整个窗口的 CPU/GPU 因果链。

### 收尾

原始 PROF、sidecar、PCM tar、cadence/通道统计、原生 SQL 结果、截图、日志与设备
包 SHA 保存在 `build/validation/kh3-adaptive-sync-20261024/installed/`。
PCM 已结束；归档逐文件 SHA 核对后仅清理本次四个设备 PCM 文件。没有 debugger、
输入注入或 Streaming。保留 ring，并启用有界慢帧留档：2 倍基线且≥33 ms、
pre=5/post=2、5 秒冷却、最多 8×8 MiB；当前暂停会话继续保留，供用户游玩验证。
