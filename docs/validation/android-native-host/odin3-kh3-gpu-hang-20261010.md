# Odin3 KH3 GPU hang snapshot（2026-10-10）

用户报告的触发：loading 完成后按确定。当前证据能确认 shadps4 的 GPU 提交未完成，**还不能证明具体 shader、资源或同步操作是根因**；未提交 GPU 行为规避补丁。

## 1. 原始现场

目录：`build/validation/odin3-kh3-gpu-hang-20261010/`。

- `kgsl-snapshot.bin`：15,429,760 字节，SHA-256 `74db4e95f15d1ceffe04bab78bc05d64e75b9651f133c5506725c647a9759eba`。
- OS 时间 `1791644526`，北京时间 **23:02:06**，与 dmesg monotonic `125329.385681` 的 hang 同一窗口；23:02:08.446 host 等待完成返回 ErrorDeviceLost 是故障后的结果。
- 同次启动 boot ID `c322cd23-e6c8-46f8-8eb7-ff032d7162db`；kernel 明确绑定包 `com.shadps4.android` / PID 5490 / context 14 / timestamp 32414。snapshot OS 的 5682 与 host Guest-1 TID 相符，不另猜一个进程。
- GPU chip `0x44050000`；`msm_kgsl.ko` Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce`。已保存模块，尚未映射到完全相符的 kernel 源码；不能套 Swan 的芯片专属规则。
- 1201 sections、END 存在，OS/EVENTLOG/MEMLIST/IB 等齐全；完整性不等于所有相关 GPU 内存均被捕获。
- 当时 Turnip 为本地旧预构建 `351a4847a0`。随后当前分支 `2b8ed3fc20` 仍复现故障，不能把旧构建当作充分解释。

## 2. 未完成的工作

- OS context 表 index 13 的 queued/consumed/retired = **32419/32414/32413**，与同窗 kernel context 14 + ring marker 对应。32414 已消费未退休，后面还有五个排队提交。
- RB section 1166，VA `0x1ff8000139000`。唯一 rbase 匹配的 preemption record 1174 给 CP read/write **5533/5588**；header read 5528，二者差 5，分别保留，不混用。
- ring index 5525 的 IB `0x41baa5b000`（27482 dwords）匹配 BR CP cursor；5570 的 context 14 EOP timestamp 32414 尚在 pending 尾部。
- context 3 的 pending timestamp 1950942240 是另一队列后继；其 consumed=retired 1950936000，不优先作为卡住的执行者。
- 三条 HFI 队列无 pending dword；历史消息不能当作正在等待的 GMU 请求。
- BR IB1 initial/remaining = 27482/21978（取指进度 5504 dwords），IB2 `0x41402ba980` 44 dwords 已取完。ROQ 有预取，**未做 A6xx 的 rptr 减 ROQ 修正，也未把取指位置宣称为故障 PC**。

## 3. 命令和 shader 的证据边界

手动 `gpu.pm4_decode` 使用严格 parity/header 检查；自动 PM4 profile 不接受此 chip，数据库未伪装成已自动重建的完整流。

- IB1 的 IB_V2 与 GPU_OBJECT 重复范围逐字节一致；IB2 亦一致，ptbase 相同。
- 主 IB 中 draw、blit、detile compute 混合存在。当前抓到的 A2D MVC 寄存器更接近前面的 768×1 copy，而不是取指尾部稍后的 2816×1 copy，不能简单归因给“最后一条拷贝”。
- `ib1-current.json` 因 maxPackets=4096 只解到 15441 dwords；涵盖所查的 5504 取指窗口，但不是全 IB 解码。`ib2-current.json` 是纯 compute pipeline state。
- 已检查捕获的 VS/HS/DS 候选；HS 只有一个在分支之前的无条件 barrier，未见后跳循环。其形式类似 RectList 的辅助 tessellation shader，但未完成精确管线归属。
- CS MVC candidate 的 config/instruction count 为 0，不按活动计算 shader 解释；L0 无匹配，也没有目标芯片可验证的活动 wave PC。
- 未见同窗 SMMU page fault；这不能排除所有内存/同步错误。原始 `Fault id:2` 不在缺少匹配源码时扩写为某个特定 fault 枚举。

## 4. 当前驱动的复测

当前驱动 SHA `ba53ae858c03dc7a0760d4cf6a81d6f32cca519514b8fb0900a5151d734c19b2`，运行时日志确认实际加载。

| 时间（北京） | 证据 | 结论 |
| --- | --- | --- |
| 23:23:47 左右 | kernel `126630.533991`，PID13288/context14/ts32150，MISC GPU hang；此前画面在 loading | 当前驱动仍可复现 hang；该时段用户也曾操作，不能宣称自动输入完整复现了其确定键步骤 |
| 23:25:20 | kernel `126723.345570`，PID14352/context6/ts5534，`CP Protected mode error WRITE addr=0` | 另一次命令处理器保护写故障，不能直接等同于原来的 shader hang |
| 23:29:24 | kernel `126967.225152`，PID15281/context14/ts5528，同样 protected WRITE addr=0 | 取得与该次故障匹配的新 snapshot，见下 |
| 23:31:14 | ActivityManager 明确 `Force stopping ... from pid 16297`；本机另一个 `tools/turnip_mesh/run_cts.py --serial 6edb0593` 正在运行 | recorder-off 对照被外部停止，不能评估 A/B；也不能把这次退出记为应用崩溃 |

第一次从 sysfs 再读取的 `current-driver/kgsl-snapshot.bin` 实际 OS 秒数 **1791645596（23:19:56）**，绑定更早的 PID11712/ts5493；其 SHA `919264d31e97e785fdd237bcf1ded63385ad437bee2356a99b87004e85811906`。旧 frozen snapshot 尚未读出期间，后续 fault 没覆盖它。**不拿这个文件解释 23:23 或 23:25 的故障。**

匹配 23:29:24 的证据在 `current-driver/repro3/`：

- snapshot OS 秒数 1791646164；Guest-1 TID15352，kernel PID15281/context14/ts5528；context 表 index11 queued/consumed/retired = **5529/5528/5527**。
- BR IB1 `0x41861be000`，15002/14042 dwords；IB2 `0x418e4d6e88`，304/0。BV 取指远在前方，不能当作已执行。
- IB1 15002 dwords / 4111 packets、IB2 304 dwords / 25 packets 完整严格解码，无 header 警告。IB1 dword730 调用上述 IB2，IB2 是一个全屏三角形 draw 的状态设置。native protected-write 报错不能仅凭“最后抓到 IB2”定位到这个 draw。
- IB2 引用的已捕获 draw-state 块逐个解码，未找到直接写寄存器 0 的包。重复 GPU_OBJECT 1194/1196 完全一致，SHA `cdecbaafa6ef8cb641be936910cef7819189971bff63740c21aa349ce2be833e`。
- 三个引用范围未捕获：`0x40328df000`（39 dwords）、`0x40328df09c`（65）、`0x40329e5b4c`（5）；前两个对应 program draw-state。相关 allocation 在 MEMLIST 存在，不把“没抓到 bytes”说成“已释放”。
- RB header read/write 704/872，匹配 preemption record 416/529，明显不同；不能把该 preemption record 的旧尾部直接当成 ts5528 的活动尾部。CP cursor 和 kernel fault 单独保留。
- `ib1.json`、`ib2.json`、`draw-state-groups.json` 保留手动解码输出。

## 5. 工具限制与后续入口

安装的 snapshot 数据库 writer 对 value-equal MEMLIST 项生成重复 program→allocation 主键，`snapshot_analyze` 两次失败。仅在忽略目录建立 reflection harness，复用安装 parser/writer，去重完全相同的 program mapping 关系（首个现场 6 项，后续 7 项），**全部原始 MEMLIST 保留，原始 snapshot 不改**。`snapshot.sqlite` 之后用只读查询分析。没有修改外部工具仓，也没有声称生成/验证了 HTML 图。

现有证据还缺原始 hang 的活动 wave/精确 shader 归属，以及 protected-write 时缺失的 program draw-state bytes。下一轮应先保证独占设备、及时释放/读取旧 frozen snapshot，再做可重复的 recorder/驱动 A/B 或应用侧命令/管线标记采集。用户最后要求只完成离线分析并安装 APK后停下，因此本轮不再启动游戏、改变诊断开关或继续 CTS 并行实验。
