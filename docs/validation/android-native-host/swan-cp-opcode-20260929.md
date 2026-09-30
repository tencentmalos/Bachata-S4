# Swan 血源 GPU opcode 故障：跨页与抢占对照、devcd12–15

## 概要

| 项 | 案发值 |
|---|---|
| Spider | not present |
| Meego | not present |
| Slardar OS | not present |
| 设备 | Swan / PB3110PGL6240001G / Adreno 840 |
| 案发时间 | kernel uptime `95667.383071 s`；原始日志墙钟 `[10:05:20]`；snapshot OS seconds `1790589921`。设备墙钟与 host 2026-09-29 不一致，不将两者混排；首选 uptime |
| boot | `20c5024e-15c5-4f6a-981f-7630a933df4c` |
| ROM | `Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`；build tag `20260922015006` |
| kernel / KMD | `6.12.58-android16-6-maybe-dirty-4k`；加载的 msm_kgsl Build-ID `37b31e3606d5e5827de3cb5154312f44d6827c95`；尚未映射至准确源码提交 |
| 后续 A/B 结论 | `sds_page_align` 与 `kgsl_preempt_rb` 均有实机失败反例；最新 faultcount 15，USB 断开后用户选择先交付离线分析和候选包。fine-grain 候选仅编译与主机检查通过，设备清理待重连，详见末节 |
| testcase / 触发动作 | 血源 The Old Hunters Edition，用户报告再次 DeviceLost；此前 session Running，533 flips，随后 Failed/BackendFailed，698 flips。未逐帧记录本次故障前的用户动作 |

### 进程与组件版本

| 角色 | process / PID | package / component | versionName | versionCode | commit / Build-ID / codePath | 版本证据 |
|---|---|---|---|---:|---|---|
| SPR | com.pico.spatial.runtime / unknown (source missing) | com.pico.spatial.runtime | 6.2.0.0-mol.20260919 | 60200012 | `/apex/com.pico.spatial.runtime_service/priv-app/SpatialRuntimeService@BQ2A.260122.002-BP2A.250705.008`；BuildCommit unknown | 同 boot 的 `fault-01/package-identity-full.txt` |
| XRRuntime | com.pico.xr.openxr_runtime / unknown (source missing) | com.pico.xr.openxr_runtime | 2.2.0 | 202000762 | `/product/priv-app/XRRuntime`；源码映射 unknown | 同上 |
| 出错进程 | shadps4.android / 23274 | com.shadps4.android | 0.1.8 | 26081400 | 主仓 b0778a7f + 未提交截图接入；APK `e7e85584…`，host `2da5cbc2…` | 同上、案发 `debug-status.txt`、前轮安装核验 |
| UMD | Turnip in PID 23274 | Mesa 26.3.0-devel | 86ca472fc2 + Mapper5 UBWC 修复 | N/A | 驱动 SHA256 `92d6582b4bd806a87e0b7486a1f0aebc1a50c1cae96d192e62600b779a6d9e82` | 案发 DebugBus；不是 alloc64/a1/FDM 候选 |

本报告原始材料根目录：`build/validation/swan-cp-live-20260929/`。可提交的摘要及哈希清单位于 [evidence](evidence/swan-cp-opcode-20260929/raw-manifest.json)。**本轮未证明 DeviceLost 已修复；已完成新现场保存、跨四份快照对照及可开关候选包；后续实机对照见末节。**

## KGSL 错误证据

### 检索覆盖

- `fault-01/dmesg.txt` 覆盖 uptime 93733.325885–95740.247326；随后 `dmesg-after.txt` 覆盖 94619.104583–96790.946356，两者合并覆盖首 fault 前至少 60 s、后至少 120 s。全量原文件保留。
- 检索 CP opcode、GPU PAGE FAULT、SMMU/IOMMU fault、Fault id、GMU/HFI timeout/reset、hwsched fault、BR/BV、waittimestamp 等。[完整表达式和计数](evidence/swan-cp-opcode-20260929/kernel-search.json)。全量命中另存 `*.kgsl-hits.txt`；宽泛 ringbuffer 命中主要是充电器日志，不能统计为 GPU fault。
- 同 boot 更早 GPU snapshot 文件仍在设备，案发 dmesg 的更早部分已滚动。未获取系统轮转 klog/pstore，不能宣称覆盖整个 boot。Android logcat 仅作补充。
- 案发内核中可见一次本次 CP opcode error，之后 snapshot/recovery；当前分析窗未见更早的 GPU PAGE FAULT。准确 KMD Build-ID 已保存，但本地参考源码尚未与该 Build-ID 精确绑定，所有结构/寄存器解释保持 `semantic_only_target_build_unverified`。

### KGSL 错误序列

| uptime / 相对首 fault | 事件 | 字段与角色 |
|---|---|---|
| 95667.015958 / −367.113 ms | HYPERHOLD GPU write zram begin | memcg name=10153，回收另一个应用；时间相关的前驱候选 |
| 95667.383071 / 0 | CP opcode error | opcode=0，首个已见硬件错误 |
| 95667.383111 / +0.040 ms | 故障 context | PID23274 / ctx48 / ts3927，明确关联 shadPS4 |
| 95667.567030 / +183.959 ms | GPU write zram end | 249265 pages / 1020989440 bytes；不是 shadPS4 UID10152 |
| 95667.597903 / +214.832 ms | snapshot created | devcd12，正常持久文件已拉回；后出的采集事件 |
| 95667.599024 / +215.953 ms | Suspended GMU | 恢复阶段；不能倒置为首因 |

### 原始 KGSL 日志块

来源 `fault-01/dmesg.txt:47895` 起，原始墙钟 `[10:05:20]`，uptime 95667.383071。回收线程的交错行也保留；[上下文摘录](evidence/swan-cp-opcode-20260929/first-fault-context.txt)。

```text
[95667.383071] [10:05:20](6)[3779|gmu_f2h]adreno-gen8-gmu 3d37000.qcom,gmu: CP opcode error interrupt | opcode=0x00000000
[95667.383099] [10:05:20](0)[1433|kgsl_hwsched]kgsl kgsl-3d0: Fault id:2 and GX is ON with Curr GPU freq:902000000
[95667.383111] [10:05:20](0)[1433|kgsl_hwsched]kgsl kgsl-3d0: shadps4.android[23274]: ctx 48 ctx_type ANY ts 3927 policy 2
[95667.383115] [10:05:20](0)[1433|kgsl_hwsched]kgsl kgsl-3d0: shadps4.android[23274]: cmdline: com.shadps4.android
[95667.383180] [10:05:20](0)[1433|kgsl_hwsched]kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004 gfx_bv_status 00880004
[95667.383183] [10:05:20](0)[1433|kgsl_hwsched]kgsl kgsl-3d0: BR: rb 0a8c/0b34 ib1 00000041BF6A6000/0a19 ib2 00000041BF626A10/01a4 ib3 0000004146017878/0000
[95667.383186] [10:05:20](0)[1433|kgsl_hwsched]kgsl kgsl-3d0: BV: rb 0a8c/0b34 ib1 00000041BF6A6000/6ebc ib2 000000415A8B7000/0000 ib3 000000415A8A7020/0000
[95667.567030] [10:05:20](1)[2262|lmkd][HYPERHOLD]GPU write zram end, memcg[id=12, name=10153]. reclaimed 249265 pages(1020989440 Bytes), took 552 ms
[95667.597874] [10:05:20](4)[1433|kgsl_hwsched]kgsl kgsl-3d0: GPU snapshot froze 4488Kb of GPU buffers
[95667.597903] [10:05:20](4)[1433|kgsl_hwsched]kgsl kgsl-3d0: GPU snapshot created at pa d3c00000++0x662630
[95667.597909] [10:05:20](4)[1433|kgsl_hwsched]Entry name already exist
[95667.599024] [10:05:20](0)[1433|kgsl_hwsched]adreno-gen8-gmu 3d37000.qcom,gmu: Suspended GMU
```

### 字段解读和证据边界

devcd12 文件 13654316 bytes，SHA256 `75c4f3150191b371d9c30d30ad52cff5a05f5286e4f554ea4ed025924f8eec5f`，有 END、零缺尾、1451 sections。MCP expert SQLite 解析完整；独立 verifier 核验 snapshot 与 31 个 GPU payload 哈希。

Snapshot OS processId=26724 不是把 victim 改成另一进程的依据；进程归属使用同窗 KMD 的 PID23274/ctx48。Snapshot 的 OS/MVC/EVENTLOG/IB 依次采集，非原子；KMD 首报与 MVC 中 IB remaining 不同，分别保留，不能当成精确 fault PC。ROQ 区间及旋转为数据匹配推断，未得到准确固件文档证明。

本次为 opcode error，不是已观察到的 page fault；以下字段避免把命令地址误写为页故障地址：

| 字段 | 本次值与边界 |
|---|---|
| fault VA/address | unknown (source missing)；没有 PAGE FAULT VA，IB2 base不是fault VA |
| read/write access and fault type | not applicable：已见CP opcode error，未见translation/permission/read/write页故障报告 |
| TTBR0/ptbase | snapshot OS/IB `0x00000009cc410000`，只表明捕获时页表，不推导ASID/VMID |
| faulting block / FAULTING BLOCK | unknown (source missing)；CP opcode interrupt不是SMMU client字段 |
| nearby mapping/MEMLIST | IB2位于 `0x41bf626000` / 524288 bytes分配中，证明捕获时MEMLIST包含；不证明采集前始终有效或缓存一致 |

## 四次故障共同落在 4 KiB 边界

[独立对照结果](evidence/swan-cp-opcode-20260929/pending-roq-comparison.json)；可复跑脚本 [compare_pending_roq.py](evidence/swan-cp-opcode-20260929/compare_pending_roq.py)。每份均有唯一最高分 ROQ 对齐；所推断的 pending 区间全部为零。

| snapshot | BR IB2 | ROQ 连续零区起点 | 推断 pending dwords | 其中 CPU 快照非零字 |
|---|---|---|---:|---:|
| devcd3 | 0x413c71653c | 0x413c717000 | 194 | 175 |
| devcd4 | 0x41cad76a10 | 0x41cad77000 | 114 | 102 |
| devcd9（alloc64 候选） | 0x05806a10 | 0x05807000 | 114 | 76 |
| devcd12（86ca + 截图修复） | 0x41bf626a10 | 0x41bf627000 | 114 | 102 |

devcd12 的 CPU IB2 包含 533 dwords / 38 条完整 PM4。第379字（VA `…6ffc`）为 `CP_SET_DRAW_STATE`、payload39字，下一条头在第419字（`…709c`）。按对齐读取的 ROQ 从第380字（恰为新页 `…7000`）到结尾连续153字为零；下一条应有的 `0x70438003` 也为零。BR/BV IB1 匹配区域没有非零差异。

这比单看 opcode=0 更具体：可疑点是跨页后的命令可见性／预取／恢复，而不是 CPU 命令中原本编码了非法头。仍无法仅凭先后复制的快照排除录制／重用时序或 CPU 与 GPU 缓存问题。**低4GiB时同样出现，alloc64不能作为已证实修复。**

## 回收线索与下一步区分

UID10153 对应 `org.xr3ds.xr3ds.relWithDebInfo`，shadPS4 UID10152。当前一次 fault 落在前者 GPU 回收的552ms内。参考源码 `kernel_platform/common/mm/memcg_control.c` 经 memcg task枚举回收指定进程；`vendor/bytedance/kernel/gpu/memory/kgsl_reclaim.c` 以 process锁、cmd_count 和 VBO 引用保护解除映射。还没有证据表明它误回收了 shadPS4 的 BO，也没有准确 KMD 源码映射；不得直接宣布 Hyperhold 为根因。

补采使用128MiB独立 tracefs instance，35事件（提交/退役/故障/抢占/alloc/free/map/reclaim），未改全局 trace开关。repro-02 trace保留1908187 / 写入2588589事件，覆盖有环覆盖，不能冒充从启动完整记录；没有 fault/reclaim 事件。此轮只推进到菜单，随后前台切到 Azahar，手柄请求明确返回 not_focused。已冻结、拉回43MiB压缩trace并释放自有instance；没有停止另一个应用。下一轮需同一包、同一路径验证开关两侧，并把故障页对应BO的 alloc/free/reclaim 与故障窗口关联。

## 候选实现、验证和交付

Mesa `tu_cs.cc/.h` 增加 `sds_page_align`，经现有 `TU_DEBUG` 解析，默认关闭。仅针对可增长 CS 的 `CP_SET_DRAW_STATE`：先预留原包空间，遇跨4KiB时再预留少量NOP空间，BO切换后重新核对地址；NOP吞掉页尾，让原包从下一页开始。固定大小的外部/substream不扩容，原包payload与顺序不变。前4次填充打印原地址/长度，便于证明实机分支执行。原 Mapper5 截图修复保留。此候选是针对跨页现象的对照，尚不是已验证修复；如果下一页仍取零，页对齐也可能无效。

- Android Turnip、host、APK 构建成功；从APK解包核验 host 与 mainline driver SHA。
- [测试输出](evidence/swan-cp-opcode-20260929/page-align-test.txt)：196608 cases / 1584 padded / 65536 rollover / 4752 crossing controls / 0 failures，UBSan无报告。测试直接抽取生产函数、使用真实PM4头构造/解码，遍历1024个页内字偏移、3–96字payload、开关及external/grow；allocator为有边界检查的测试替身，不替代真实KGSL或硬件验证。
- ASan+UBSan版本在本机持续运行未及时完成，已主动SIGTERM终止；不计为通过。
- 候选 APK：`build/validation/swan-cp-live-20260929/candidate/shadps4-b0778a7f-sds-page-align.apk`，SHA256 `bfc3e34609f1ca8436cb2ddfc31c38e42226efda528730cc0f976dd81cd1c625`。
- 候选驱动 `c35946a8758de0400899b69f28b2158a06931eccf7d9d44cd454022a5fb81b23`；完整本地驱动zip、组合patch、构建脚本和包身份位于同目录。后续实机 A/B 已安装此候选，并逐进程切换属性；结果见下节。
- A/B入口为 `TU_DEBUG=sds_page_align`；Android对应 `debug.mesa.tu.debug`，需重启进程。该属性是全局Mesa调试入口，操作前保存旧值，完成后恢复；安装前为空。正式锁文件和两处固定SHA保留已发布86ca，不将无发布URL的本地候选加入正式依赖。
- 未提交/推送；a1新增FDM路径黑屏尚未复测，不与本次CP候选合并结论。


## 实机 A/B：候选未通过（2026-09-29）

用户授权继续使用 Swan 前台、安装候选及重启测试。四轮使用同一 APK `bfc3e346…`、host `8bfd013b…`、驱动 `c35946a8…`，同 boot、同 ROM/KMD；只在新进程启动前切换 `debug.mesa.tu.debug` 为空（A）或 `sds_page_align`（B）。候选仍基于86ca + Mapper5截图修复，不是a1/FDM驱动。B1/B2均有本进程四条实际填充日志，A1/A2无填充日志。完整身份、逐次状态及输入回执见 [A/B摘要](evidence/swan-cp-opcode-20260929/ab/ab-summary.json) 和 [实验条件](evidence/swan-cp-opcode-20260929/ab/experiment.json)。

| 组 | 缓存 | PID / gen | 状态记录窗 | 最后正常记录的 flips | 结果 |
|---|---|---|---|---:|---|
| A1 off | 暖 | 25923 / 1 | 290.021 s | 8437 | 猎人梦境，未见新 fault |
| B1 on | 暖 | 26142 / 1 | 409.400 s | 12297 | 猎人梦境，未见新 fault；实际填充分支已执行 |
| A2 off | 冷 | 29580 / 1 | 300.086 s | 8638 | 猎人梦境，转镜头及往返短移后未见新 fault |
| B2 on | 冷 | 3617 / 1 | 最后正常状态161.926 s；首fault距首次Preparing 165.032 s | 4488 | **GPU fault，faultcount 12→13，进程SIGABRT退出** |

“冷”仅指 host recipe 与 VkPipelineCache 目录在进程启动前缺失，不声称所有系统/驱动缓存均冷。两组均报告 `preload: 0 stored`、`at startup: no file (0 bytes)`，进梦境时分别新建157条graphics + 30条compute。原缓存先移开并备份，两轮新缓存各自留证。暖缓存不重置同一缓存，因此不是字节完全相同的缓存输入。

四轮都有正常编码截图，共21次，全部有编码样本及EOS；截图保存在原始证据目录，未改走lossless。B2最后正常截图编码PTS为99086.132688–99087.452885，距离首fault约18.3秒，故障时没有正在运行的截图请求。B2仅发送了三个确认输入（启动提示、离线、继续）；准备转镜头时身份检查已失败，第四次输入未发送。A1进入场景有记录外输入，B1两个转镜头请求提前被后续请求替换，不能把四轮说成严格自动回放或性能基准。

### devcd13 的 KGSL 错误证据

故障 PID3617 / gen1 / UUID `875ab65ed3a978d5e31825f3a88a88ab`；ctx48 / ts23573 / rb2。OS snapshot 的 processId20808是捕获字段，victim归属以同窗内核及trace为准。组件ROM、SPR/XRRuntime版本沿用本报告同boot身份，未更新组件。UMD为开启SDS候选的c35946a8；native tombstone host Build-ID `a000bc6bf988daf710f86942a72a6836031d688b`。

`fault-13/dmesg-after.txt`覆盖97037.893305–99349.988643，覆盖首fault前60秒和后120秒。按 [检索表达式、命中与覆盖](evidence/swan-cp-opcode-20260929/ab/devcd13/kernel-search.json) 检查：一条opcode error、一条Fault id，未见同窗GPU PAGE FAULT/SMMU/IOMMU fault或GPU write zram/reclaim。完整原始日志和tombstone均保留，不能把事后恢复的Suspended GMU倒置为根因。

```text
[99105.724781] [11:02:39](3)[3779|gmu_f2h]adreno-gen8-gmu 3d37000.qcom,gmu: CP DDE BR opcode error | opcode=0x00000000
[99105.724808] [11:02:39](2)[1433|kgsl_hwsched]kgsl kgsl-3d0: Fault id:2 and GX is ON with Curr GPU freq:902000000
[99105.724816] [11:02:39](2)[1433|kgsl_hwsched]kgsl kgsl-3d0: shadps4.android[3617]: ctx 48 ctx_type ANY ts 23573 policy 2
[99105.724820] [11:02:39](2)[1433|kgsl_hwsched]kgsl kgsl-3d0: shadps4.android[3617]: cmdline: com.shadps4.android
[99105.724870] [11:02:39](2)[1433|kgsl_hwsched]kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004 gfx_bv_status 00880004
[99105.724873] [11:02:39](2)[1433|kgsl_hwsched]kgsl kgsl-3d0: BR: rb 1d53/1dfb ib1 00000041E28F9000/2a00 ib2 00000041E288A3F8/03e6 ib3 0000000000000000/0000
[99105.724877] [11:02:39](2)[1433|kgsl_hwsched]kgsl kgsl-3d0: BV: rb 1d57/1dfb ib1 0000004032D39000/0000 ib2 0000000000000000/0000 ib3 0000000000000000/0000
[99105.926962] [11:02:39](4)[1433|kgsl_hwsched]kgsl kgsl-3d0: GPU snapshot froze 3840Kb of GPU buffers
[99105.926979] [11:02:39](4)[1433|kgsl_hwsched]kgsl kgsl-3d0: GPU snapshot created at pa d3c00000++0x644a50
[99105.928249] [11:02:39](1)[1433|kgsl_hwsched]adreno-gen8-gmu 3d37000.qcom,gmu: Suspended GMU
```

[原始上下文](evidence/swan-cp-opcode-20260929/ab/devcd13/kernel-fault-context.txt)。随后host日志报 `GetRenderFrame: Device lost during waiting for a frame`，tombstone为该断言引起的SIGABRT；不是已验证的UIStop/BackendFailed退出。

本次仍无PAGE FAULT VA、访问方向或SMMU faulting block；不得把IB2 base写作fault VA。snapshot ptbase=`0x0000000b24416000`，MEMLIST含IB2所在`0x41e2879000 / 524288 bytes`分配，仅证明捕获时存在。

### 新现场与旧现场的区别

1. devcd13文件12,868,724 bytes，SHA256 `e361f44aa55cfd4ddec87907bd89828355e1b1ab4fbc4c9d9b132095401db4ca`，1444 sections、END完整、缺尾0；独立校验25个GPU payload全部匹配。[核验与ROQ结果](evidence/swan-cp-opcode-20260929/ab/devcd13/verified-roq.json)。
2. **BR IB2这次没有捕获到“跨页后为零”**：1514 dwords可解出132条完整PM4；推断ROQ旋转唯一，464个非零字匹配、零差异、其他差异0。BR IB1的216个非零字也全匹配。推断消费位置998落在SDS payload内，不是包边界，不能据此给出精确故障PC。BV对齐无唯一解，报告中的BV零差异候选不能作为有效结论。
3. 故障IB2没有NOP，也没有跨页的SDS包。虽然候选确实执行过，**这份失败IB2本身不需要SDS填充**。[静态draw-state检查](evidence/swan-cp-opcode-20260929/ab/devcd13/draw-state-check.json)：140个唯一引用，104个有payload并通过包类型/长度检查，36个未捕获；4个state区间跨页，其中一个未捕获。没有证明全部引用、生命周期或DDE实际读到的字节正确，不能排除DDE取数、状态恢复或未捕获的state问题。
4. B2独立trace保留/写入均1,145,968条，无环覆盖，覆盖98923.191967–99105.720468（182.529秒），含本进程ctx48创建到fault。整个窗口没有`kgsl_reclaim_memdesc`/`kgsl_reclaim_process`；因此devcd12的“恰逢另一应用回收”不能作为所有opcode故障的必要条件。
5. [故障前10ms追踪](evidence/swan-cp-opcode-20260929/ab/devcd13/trace-fault-10ms.txt)里，99105.720388记录抢占完成、从rb1切回rb2，80µs后记录ctx48/ts23573 fault。F2H事件送达时间不是精确GPU执行时刻，这只是定位DDE/抢占恢复的下一条线索，尚不能归因于抢占。未修改共享GPU抢占配置或刷KMD。

结论：**当前SDS跨页候选未通过实机稳定性验收，应保持默认关闭。** B2的错误子类型是`CP DDE BR`，不能直接宣称与devcd3/4/9/12底层机制完全相同，也不能以一个B2样本断言候选引入回归；但“已经修复当前GPU崩溃”的结论已被反例否定。下一步需要围绕DDE state读取及抢占恢复补齐缺失payload，而不是继续把SDS对齐当作完成修复。

### 终态与可复核材料

原始目录：`build/validation/swan-cp-ab-20260929/`；[文件哈希清单](evidence/swan-cp-opcode-20260929/ab/raw-manifest.json)。各轮trace已冻结、收集并释放，全局trace未修改；21次编码截图全部结束，42个本轮设备截图/metadata临时文件已移除，本地证据保留。B1/A2经界面Stop确认`user_stop`；A1未点Stop确认就force-stop，已作为操作局限记录；B2为故障退出，最后无shad进程。

`debug.mesa.tu.debug`恢复原空值；原缓存1264个文件逐SHA一致，A2/B2新缓存留在host证据中。存档33个文件无增删，4个血源userdata/backup随会话推进变化；未外部编辑或回滚存档。[清理状态](evidence/swan-cp-opcode-20260929/ab/cleanup.json)、[存档核查](evidence/swan-cp-opcode-20260929/ab/save-check.json)。设备保留候选APK，但下次启动SDS默认关闭，Mapper5截图修复仍在；正式源码锁和SHA保持已发布86ca，未提交/推送，也未测试a1/FDM黑屏是否修复。


## devcd13 深挖：DDE 当前状态与 context 抢占方式实验

### DDE 当前 SDS 引用

继续直接核对同一份 devcd13 SQLite 与原始 payload，未重新采集或重建数据库。[寄存器/字节摘要](evidence/swan-cp-opcode-20260929/preempt/devcd13-dde-active-state.json)。DDE_BR MVC 的 `CP_SDS_BASE` 为 `0x41e2984efc`，初始长度 33 dword、剩余 32 dword。该地址精确对应故障 IB2 中 dword 884 的 group 10（`TU_DRAW_STATE_CONST`），头为 `0x40ab3020`，即一次向 `SP_SHARED_CONSTANT_GFX[0..31]` 写 32 个字的 type-4 包。开头四个值为 960、540、960、−540，随后零值是常量数据，不可直接当作坏 opcode。整个引用范围为 `[0x41e2984efc, 0x41e2984f80)`，不跨 4 KiB 页。

DDE ROQ 内反复出现的另两个地址 `0x41e2984ed4` 和 `0x41e29859e4` 均能对应 group 17（LRZ/depth plane）、各 10 dword、5 个合法长度的 type-4 包，两个 payload 均已捕获。DDE FIFO/ROQ 是内部格式，不能套 BR IB2 的旋转布局；也不能把非原子 MVC 的“初始−剩余=1”当作精确 fault PC。上述核对只证明捕获时内存中的引用/长度/字节自洽，不能证明故障时 DDE 实际消费的内容正确。

[完整 pre-fault trace 中两块命令 BO 的事件](evidence/swan-cp-opcode-20260929/preempt/devcd13-target-bo-events.txt)：`0x41e2879000` / 512 KiB 在 uptime 99074.658177 分配，`0x41e2939000` / 2 MiB 在 99074.659217 分配，均在 shad PID3617 的 GpuComm 线程，flags `0x10c0000`（GPU readonly、write-combine）。到 fault 99105.724781 前未见这些地址被释放，完整 trace 无任何 reclaim 事件。这不支持“本次由 BO 释放/回收造成”的解释，但不排除用户态复用、CPU 写入或 GPU 内部恢复问题。

### 新的单变量候选

为检验抢占关联，仅增加默认关闭的 `TU_DEBUG=kgsl_preempt_rb`：KGSL context 创建时请求 `KGSL_CONTEXT_PREEMPT_STYLE_RINGBUFFER << 25`，记录请求/返回 flags、context 和 ioctl 结果，保持错误返回的 errno。关闭时原始 flags 和提交路径不变；上一版 `sds_page_align` 在本轮两组均关闭。未改变共享抢占 sysfs、context 优先级、固件或 KMD。

本地参考 KMD clean HEAD `cd8a361e34bbd7e0bbe0eac9bdf3098234bec32d` 的 `adreno_drawctxt_create` 保留 style bits，Gen8 `send_context_register` 将其传给 HFI；查询使用 `git show HEAD:path`，未把其他会话的 dirty XR probe 当作本次源码。[源码身份及摘要](evidence/swan-cp-opcode-20260929/preempt/kernel-source-reference.json)。加载 KMD 的 Build-ID 尚未匹配准确源版本，解释仍为 `semantic_only_target_build_unverified`。请求名 ringbuffer 不能直接解释为“关闭全部抢占”或已知的具体硬件安全边界。

实验包 APK `2b0e7b7a5aa90ad4e591648d77ef2ef0b70079d3e73ea93bae05502341cda9f2`，driver `13625257e968e9d81a2197dc66850000c82718f7940926b40106b6a1f680acaa`，host `0a378a3ba261c421063d754d5eb9730e4e0f01c19302863811fc83db18015071`。安装后的 base.apk 哈希已回读匹配，运行时 DebugBus 驱动 SHA 一致；本地包独立保留于 `build/validation/swan-cp-preempt-20260929/candidate/`。正式源码的驱动锁及两处 SHA 仍为可下载的 86ca；默认构建 host/APK 已重新编译并核验包内 driver/host 一致。

本轮数据目录：`build/validation/swan-cp-preempt-20260929/`。冷缓存含义仍仅是启动时 host recipe/VkPipelineCache 目录不存在；原目录完整暂存，存档仅通过游戏推进。开启组 PID16380 / generation1 / UUID `6f6d86b4e65b82a5f9a933e5795f73fa`，context51，日志确认 `requested_flags=0x2000013 returned_flags=0x2008052 result=0`，返回 style=1、priority=8。B1 的早期日志已滚出最终 logcat，留存文件明确标为工具输出摘录；B2 同类请求日志有原始文件。以下补齐实机结果。

## Context 抢占方式实机对照：ringbuffer 也未修复

三轮同 APK `2b0e7b7a…`、driver `13625257…`、host `0a378a3b…`、boot、ROM 和 KMD；仅在新进程前切换 Mesa 属性，SDS 对齐均关闭。[实验条件](evidence/swan-cp-opcode-20260929/preempt/experiment.json)、[逐轮摘要](evidence/swan-cp-opcode-20260929/preempt/ab-summary.json)。A1 后再次读取的加载模块 note 仍为 Build-ID `37b31e3606d5e5827de3cb5154312f44d6827c95`，原始 note 保存在 fault-14 目录。

| 组 | 请求 / PID / ctx | 观察与场景 | 结果 |
|---|---|---|---|
| B1 冷 | ringbuffer / 16380 / 51 | 首次 Running 到最后 Running 610.987 s，303→18424 flips；梦境转镜头、往返短移 | faultcount 13→13；随后界面 Stop 确认 user_stop |
| A1 冷 | 默认 / 20698 / 51 | 首次 Preparing 到首 fault 481.690 s；最后 14019 flips；梦境转镜头、往返短移 | **faultcount 13→14，CP DDE BR opcode=0，GetRenderFrame 断言 SIGABRT** |
| B2 冷 | ringbuffer / 20901 / 51 | 首次 Running 到首 fault 362.263 s；最后 10714 flips；最后两张截图仍为离线菜单 | **faultcount 14→15，同类 CP DDE BR opcode=0** |

B2 原始 `tu-start.txt` 确认请求和返回 style=1，不是忘开开关。只发送两个独立确认输入；同 ID 的重试是查询旧回执，没有再次注入；第三个新输入因 session 已非 Running 在发送前被拒绝。B2 路线没有推进至梦境，不能把本表当成相同路线的性能基准或比较平均故障时间。B1 单轮未失败不能推翻 B2 的失败反例。

本轮正常编码截图 **21 次**（B1 9、A1 7、B2 5），全部有编码样本、EOS 和本地 PNG；失败的截图尝试不计入。[截图核验](evidence/swan-cp-opcode-20260929/preempt/screenshot-check.json)。没有切换到 lossless 截图。

### devcd14 的 KGSL 错误证据及解码

原始 `a1-default-cold/fault-14/dmesg-after.txt` 覆盖 uptime 101486.694276–104020.448576，满足 fault 前 60 s、后 120 s；[检索式与结果](evidence/swan-cp-opcode-20260929/preempt/a1-default-cold/fault-14/kernel-search.json)中，同窗无 GPU PAGE FAULT/SMMU/IOMMU fault 或 reclaim 命中。首个已知 GPU 错误为 DDE opcode，随后 snapshot/GMU suspend 是采集与恢复，host 的 DeviceLost 断言为传播结果；不倒置因果。

```text
[103751.209756] [12:20:05](3)[3779|gmu_f2h]adreno-gen8-gmu 3d37000.qcom,gmu: CP DDE BR opcode error | opcode=0x00000000
[103751.209783] [12:20:05](1)[1433|kgsl_hwsched]kgsl kgsl-3d0: Fault id:2 and GX is ON with Curr GPU freq:902000000
[103751.209790] [12:20:05](1)[1433|kgsl_hwsched]kgsl kgsl-3d0: shadps4.android[20698]: ctx 51 ctx_type ANY ts 69982 policy 2
[103751.209884] [12:20:05](1)[1433|kgsl_hwsched]kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004 gfx_bv_status 00880004
[103751.209887] [12:20:05](1)[1433|kgsl_hwsched]kgsl kgsl-3d0: BR: rb 0931/09d9 ib1 00000041FC809000/1128 ib2 00000041FCAAB1EC/017c ib3 0000000000000000/0000
[103751.209891] [12:20:05](1)[1433|kgsl_hwsched]kgsl kgsl-3d0: BV: rb 0935/09d9 ib1 0000004032D39000/0000 ib2 0000000000000000/0000 ib3 0000000000000000/0000
```

归属 PID20698 / gen1 / UUID `66369bdf4828bdedfe7659ecb11a9388`，ctx51 / ts69982 / rb2。snapshot `kgsl-1790598005-devcd14.bin` 为 12,744,904 bytes，SHA256 `1cfef9aa784fcfb09112bb238ace75ed68594b7b96b4392ac460a6b989b82c64`；1443 sections、END、零缺尾，24 个 payload 哈希独立匹配。[可复核 ROQ 结果](evidence/swan-cp-opcode-20260929/preempt/a1-default-cold/fault-14/verified-roq.json)。ptbase `0xb6f25000` 是捕获值；未有 PAGE FAULT VA、读写方向或 SMMU faulting block，不将 IB 地址充当页故障地址。

- BR IB1/IB2 的推断 ROQ 对齐各有唯一最高分，分别 217/467 个非零字匹配、零差异；IB2 的 1395 dwords 可按包类型和长度解为 82 包。候选消费位置 380 又在 SDS payload 内，不能当作精确 PC。
- DDE 当前引用 `0x41fc8b224c`、初始 7 dwords、剩余 0，位于单页，对应 group29（动态 vertex input）。内存为一个 type-4、6 字 payload，全部非零；与 devcd13 的 group10 常量包不同。未发现这些捕获字节的包长度错误，仍不能证明 DDE 实际读到的内容或生命周期正确。
- 129 个唯一 state 引用中，93 个 payload 可捕获并通过包形状检查、36 个缺失。IB2 中有一个跨页 SDS，位于 dword897，而捕获游标只表明取到 dword869；没有证据将该跨页包列为本次故障指令。
- trace 保留 2,121,859 / 写入 3,880,544 条，发生环覆盖；所有 CPU 的共同保留起点为 103498.879231，覆盖故障前约 252 秒，该保留窗未见 reclaim。103751.200642 记录 rb2→rb0 level1，103751.205350 切回 rb2，115 µs 后记录 fault。时间是异步 F2H 到达时间，只建立关联。

B1/A1 最后 60 秒均取自所有 CPU 有保留数据的共同窗口；B1 rb2→rb0 level1 有 4134 次，A1 有 4805 次。[B1](evidence/swan-cp-opcode-20260929/preempt/b1-rb-cold/preempt-final-window.json)、[A1](evidence/swan-cp-opcode-20260929/preempt/a1-default-cold/preempt-final-window.json)。这些事件不是逐 context 独占统计，但足以说明不能把返回 style1 解读为整机不再发生该类抢占；GMU 对此请求的实际语义尚未验证。

### faultcount15：开启组再次失败，但 snapshot 尚缺

[原始内核上下文](evidence/swan-cp-opcode-20260929/preempt/b2-rb-cold/kernel-fault-context.txt)与 [覆盖检查](evidence/swan-cp-opcode-20260929/preempt/b2-rb-cold/kernel-search.json)来自自动监控已保存的 `b2-rb-cold/dmesg-fault.txt`：

```text
[104470.369166] [12:32:04](4)[3779|gmu_f2h]adreno-gen8-gmu 3d37000.qcom,gmu: CP DDE BR opcode error | opcode=0x00000000
[104470.369190] [12:32:04](5)[1433|kgsl_hwsched]kgsl kgsl-3d0: Fault id:2 and GX is ON with Curr GPU freq:578000000
[104470.369198] [12:32:04](5)[1433|kgsl_hwsched]kgsl kgsl-3d0: shadps4.android[20901]: ctx 51 ctx_type ANY ts 54656 policy 2
[104470.369290] [12:32:04](5)[1433|kgsl_hwsched]kgsl kgsl-3d0: BR: rb 1991/1a39 ib1 000000413B48A000/05ef ib2 000000413B5929EC/0000 ib3 0000000000000000/0000
```

上述行逐字来源见链接原文；只保留到 fault 后约 4 秒，不满足后 120 秒覆盖。日志采集的下一步被无效 UTF-8 异常中断，随后的拉取又遇 USB 断开；**尚无本地 devcd15 文件/哈希/数据库或 B2 trace，不声称它与 devcd13/14 具有相同的 ROQ/DDE 状态。** 已给下一轮采集脚本增加二进制落盘、每项独立错误记录、logcat 末尾 4000 行上限，避免一项解码错误阻断其余证据；该采集改动未在设备上运行。

## 离线继续定位与 fine-grain 候选

直接追查故障 IB2 的调用点得到一个重要限制：[devcd13 IB1](evidence/swan-cp-opcode-20260929/preempt/devcd13-ib1-render-mode.json) 和 [devcd14 IB1](evidence/swan-cp-opcode-20260929/preempt/devcd14-ib1-render-mode.json) 分别只有一个匹配调用（dword10748、4388），其前一个 marker 都是 `RM6_DIRECT_RENDER=1`。两份完整捕获的 IB1 中只见 marker1/8，没有 GMEM 的 `RM6_BIN_RENDER_END=7`。因此本次静态路径为 **sysmem**，不支持直接把故障归给 GMEM bin restore；也没有据此修改 marker 或屏蔽全局抢占。

参考 KMD 的 Gen8 HFI 注册明确存在 ANY/RB/FG 三个值，并原样传 style bits25–27；不是 UAPI 与 HFI 位域不一致。旧 A5xx 对 fine-grain 的解释不能直接外推到 Swan Gen8 固件。公开 Mesa 的 [CP_CCHE_INVALIDATE 等待修复](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/df96a4daa7a38d7b17ad759d1e56d5d7f20f9ed5/src/freedreno/vulkan/tu_cmd_buffer.cc)也已在当前 `tu_emit_cache_flush` 中存在，没有重复移植。尚未找到有证据支持的确定修复。

新候选只增加默认关闭的 `TU_DEBUG=kgsl_preempt_fg`，创建 context 时请求 style2。默认仍请求 style0；RB 与 FG 同时开启则明确 EINVAL，不允许错误组合成 style3。日志记录请求/返回 style，保留 ioctl errno；不改变优先级、type、命令 stream、BO 分配方式或共享 KMD 设置。意图是补齐三种可请求方式的单变量对照，**不是声称 fine-grain 必然避开恢复错误或已经修复**。

- Mesa、Android host、APK 构建通过；[40 项主机检查](evidence/swan-cp-opcode-20260929/preempt/candidate-fg/context-flags-test.json)直接提取生产函数及真实 UAPI 常量，验证默认/RB/FG 位、成功/失败、互斥、errno 和 queue ID，UBSan 无报告。ioctl 为 mock，不代替 KMD/硬件验证。
- APK：`build/validation/swan-cp-preempt-20260929/candidate-fg/shadps4-b0778a7f-kgsl-preempt-styles.apk`，SHA256 `d272bb0e422bbb531ad7b61266e75a196fbb5f341687fe7e03fbca47bed23af3`。
- 包内 driver `c1a77fde0d5b25300e55f0eb9cc59313818c9e96a707ab9b59ab6da4e37f3d82`；host `aa7a5a4375cce8c2c5a7914571242c972cf4163cf9dd53e63fea931de8df0518`。[完整身份](evidence/swan-cp-opcode-20260929/preempt/candidate-fg/apk-identity.json)。Mapper5 编码截图修复保留。
- 正式 driver lock 和两处固定 SHA 保留发布86ca；新包及驱动仅本地候选，未安装、未发布、未提交/推送。默认构建恢复后另核包内稳定驱动/host，不把候选留成无 URL 的正式依赖。

下一轮先补回 fault15 的持久化 snapshot 和冻结 trace，再恢复本轮现场。若继续测试，用同一候选分别请求空值与 `kgsl_preempt_fg`，保持 SDS/RB 关闭，确认 runtime SHA 和返回 style、逐次截图确认菜单/梦境，再按同一转镜头/短移路线多轮运行。任一次匹配 DDE fault 即为失败；有限无错观察仍不能替代长期稳定性验收。返回 style 本身不证明硬件采用了预期恢复路径。

### 当前终态：设备恢复待重连

用户选择本轮先完成离线分析与候选包。**本节覆盖前一轮“缓存已恢复、属性为空”的历史终态。** [待恢复清单](evidence/swan-cp-opcode-20260929/preempt/cleanup-pending.json)：

- 设备仍装 RB 实验包 `2b0e7b7a…`，最后属性为 `debug.mesa.tu.debug=kgsl_preempt_rb`；须恢复原空值。
- 原 cache 暂存于 `files/host/cp-preempt-original-20260929`；当前 `files/host/cache` 为 B2 测试缓存。重连先确认游戏停止，再另存 B2、恢复原 cache，并逐相对路径验证原 1264 个 SHA。存档不回滚；本轮最终存档差异尚未核对。
- B1/A1 trace 已收集并恢复，32 个对应编码临时文件已核 SHA 清除；B2 自有 instance `kgsl_dbg_0929065136_4382` 及 10 个编码/metadata 文件待收集/清理，触发器预期冻结但断连后未核验。未读取 live snapshot/dump，未修改全局抢占设置或其他应用生命周期。
- 需补取持久化 devcd15、B2 trace/host log/tombstone。最后可用 dmesg 已本地保存；不能将设备缺席写作清理成功或修复验收通过。
