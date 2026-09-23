# AYN / MHW DLC 确认、静默提示与片元 LDS（2026-09-22）

用户优先要求定位 MHW 确认 DLC 时的错误，并让 ImGui 提示默认静默、自动 OK、保留日志。本轮修改未提交；此前其他怪猎/PSVR 改动保留。MHR 重连阶段另见 [记录](mhr-device-20260922.md)。

## DLC 阻塞原因与修复

用户会话 PID12633 / UUID `c2a267f89d927bd32e59db2653292a32` 在 14:51:38 因 op228、NID `rdgs5Z1MyFw`（sceNpWebApiCreateRequest）未准入而 Guest Fault。屏幕停在“正在确认追加内容”。没有发现此错误来自 ZAR 损坏。

精确主模块 SHA `cd4e3620be7182f1d35d7a67626c3d543bd481257bf764271663e054a8c2ec9b`，main+0x4a77863 调用 CreateRequest，随后检查负返回并在 +0x4a7795f 正常清理错误。此前未实现调用使游戏无法走这条分支。

现补 WebAPI 离线请求生命周期的 9 个入口：Create/DeleteContext、Create/DeleteRequest、SetRequestTimeout、SendRequest2、ReadData、GetResponseHeaderValue/Length。复用桌面真实本地对象，校验会话所有权、字符串和缓冲区映射，发送返回真实 SignedOut；读响应保留原发送错误与输出缓冲区，不把失败变成 EOF/成功。修复桌面 DeleteRequest 错误出口漏解锁、请求/用户析构释放。在线服务及 push callback A 两入口仍未准入。

V1 APK `0668768f` / host `bb28193c`，PID22737 / UUID `c394b4a00396edcf0aa4e8ed1a71a8ec`：实测 userContext=-1，CreateRequest 返回 `0x80552904`（LIB_CONTEXT_NOT_FOUND），adapter_ok=true；游戏继续列出追加家具、模型、接待员服装等“已获得的追加内容”。这是 DLC 查询路径通过，不代表 236 个 DLC 的全部玩法验收。

## 默认静默模式

新增 System → **Silent Dialogs**，`general.silent_dialogs` 默认 true，支持全局及逐游戏覆盖，重启游戏生效。

- Error 与单 OK 信息提示走原结果/租约生命周期，直接 FINISHED + OK，不显示模态、不夺取手柄输入。
- `DIALOG_AUTO_ACK` 记录 request、user、标题、原文、mode、action、result 与 button，写入 host 日志及 Android `GuestDialog` 日志。
- Yes/No、OK/Cancel、文本输入、进度等待、存档覆盖等仍保留交互/等待，不自动作选择。游戏自己绘制的窗口不属于此设置。
- MHW 实测 `0X80550006` 和“无法连接至PlayStation™Network。”都被自动确认。游戏内 `70a-MW1` / `710-MW1` 网络消息仍由 Cross 正常关闭。

## DLC 后续闪退：真实片元指令缺口

V1 15:11:42 PID22737 / GpuComm23136 在 DS_WRITE 断言 SIGABRT，BuildID `39384129b468501ea74ba8456cc19594c93cdb46`。诊断包 `6c2fd875` / host `ee3529b0`，PID28778 再现于 shader `0x15e44dcc`。

原始 GCN 9240 B，SHA `aef5ca5a3ebfae5d70c517da6abb9573776ba2fd0fc072295f5f77cc2cac7ab0`。生产 decoder 确认 +0x670 是 `DS_WRITE2_B32`，32-bit / pair / 非 ST64 / 非 GDS，offset0/1=0/1，addr=v1。后续对为 (2,192)、(193,194)。+0x608/610 mbcnt 获取 lane，+0x614 为 lane×12；+0x1a70、+0x1af0 重新计算相同地址，读写始终对应 6 个私有 DWORD。

旧代码只接受单次 32-bit、offset 256 对齐，且直接忽略地址。现保留真实 DS 字节地址到 SSA 后：对 64 个 Guest lane 验证地址和对齐、全部访问的归属及别名，证明线程私有后用虚拟寄存器和现有 CFG/SSA 保存值。配对偏移按元素大小/ST64 换算，等偏移写仅 DATA0；GDS 继续走存储缓冲区。动态未知地址、交叉 lane、局部别名和未实现位宽明确拒绝，非完整 fragment LDS 仿真。shader binary 版本升至14。

语义参考：[AMD GCN3 ISA §10.3 / §12.7](https://gpuopen.com/download/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf)。GCN3 文档用于指令语义；实际 SICI 编码以本仓生产 decoder 和捕获字节核验，不用 GCN3 位位置套解。

新增默认关闭 `debug.shadps4.shader_dump=1`，在编译前导出原始 GCN，编译后导出 SPIR-V 至 host/shader/dumps；诊断完成恢复属性。旧 LLVM tahiti 不支持 disassemble、gfx803 不同编码的输出均不作证据。

## 定向验证

| 项目 | 结果 | 边界 |
|---|---:|---|
| AYN Msg/Error 静默模式 | 244 / 0 fail | host a7fcb1c2；原交互与 WAIT/选择继续保留 |
| Kotlin SilentDialogs | 3 tests / 0 fail | 默认、显式 false 覆盖、目录和非法输入 |
| AYN WebAPI / PSVR 服务 | 362 / 0 fail | host bb28193c；所有权、离线错误、输出保护、解锁/释放 |
| AYN fragment LDS | 467 / 0 fail | 含捕获 shader 的 87 blocks / 6 slots；插值运行参数为测试夹具，非完整游戏 pipeline 重放 |
| Qualcomm / Turnip 实际片元输出 | 各 10240 / 0 fail | 10 种读写组合、逐像素变化输入与回读；非游戏性能 |
| SPIR-V | 10 / 10 | Vulkan1.2 validator |
| V4 实际 MHW shader 0x15e44dcc | 1 / 1 | 111796 B，Vulkan1.3 validator；不能代替游戏 GPU 执行验收 |

保留第一次 captured-shader 测试因缺插值夹具而在 V_INTERP_P2 断言的无效结果；修正夹具后通过。测试失败没有解释为游戏驱动问题。最初 RSP PID15715 未命中请求断点，退出为 BackendFailed/return0，不能冒充正常 Stop；原始参数来自后续普通运行 opt-in 网络日志。

## 最终实机与剩余项

V4 APK `14158e657134d80c41435e17c1ef88197159d712f9c9fae4b4aa37b8668e3b62`、host `27df8016`、JNI `02058818` 已安装并核对设备 APK SHA，包内 host 与最终 CPU/GPU 测试相同。PID10586 / generation1 / UUID `7bb3eb688186084dd5114ea2bb06b68a` 越过 DLC 确认和旧 DS_WRITE2 断言；实际编译得到上述 shader 的 SPIR-V，后续继续编译其他着色器。

本轮仍未进入可操作关卡：15:45:23.023 返回 `BackendFailed: vkQueueSubmit failed: ErrorDeviceLost`，15:45:23.418 进程退出，exit33。最近一次状态快照仍显示 Running，guest flip8733 / host present8731 / draw830930 / queue submit29105；此快照早于终态，不作成功依据。帧计数包含前面的消息界面，不能当成已进入游戏。未捕获首个故障 GPU 命令、shader 或资源；日志中最后编译的 shader 也不等于故障 shader。保存的 dmesg 仅83 B、只有 audit_lost 行，不能作为“内核无 GPU fault”的证据。对 0x15e44dcc SPIR-V 的离线检查没有 Workgroup 存储或 barrier，但这仍不足以排除其他执行/资源问题。

完整导入清单 [imports-v4.json](mhw-dlc-silent-20260922/imports-v4.json)：1024 行、145 refused 行、142 唯一未准入项。WebAPI 请求族已准入，push callback A 等仍保留；当前实际终止点是 GPU 提交失败，不是这些未调用项。

设备测试发生任务重叠：另一任务报告只做 scrcpy/OCR 与设置导航，没有安装、驱动切换、RenderDoc 重放、GPU reset 或图形设置改动；15:43:33.524 一次设置复位 tap 可能落入新游戏，效果未核验，15:43:34.257 随即由 Guest 守卫拒绝后续输入。其最后截图为15:43:51.301，15:45:14–23 故障窗口没有该任务设备操作。保留这个输入干扰边界，不将它解释为 GPU 故障根因，也不宣称该轮是独占受控实验。

收到用户将 AYN 临时交给 OCR 任务的协调通知后，本任务暂停全部设备操作、仅做离线分析。没有为交接额外 Stop 游戏；最后已保存快照没有应用 PID/MainActivity。原只读 scrcpy 被该任务复用；后续该任务确认会话 aefe101c07a34d238a02a84be8ca2b58 因 TTL 正常退出（exit0），不再待清理。该任务新建会话由其负责。shader_dump/network_log 两个诊断属性仍为1，rwlock_log/rwlock_slot为0（四者原基线为空）；guest_debug_port/wait 为原基线0。此清理暂未完成，已告知接手任务，不能写成设备已完全复原。

设备归还后，先核对无其他活动任务与当前会话、保存存档/配置终态，再做同 APK/同驱动的独占复现，抓取首个失败提交与 GPU fault。若仍复现，按资源生命周期、shader/descriptor 输入和驱动证据继续定位；不能仅凭 ErrorDeviceLost 修改 shader 或把错误吞掉。当前无 MHW 可玩或 GPU 崩溃已修结论。

本地大证据根 `build/validation/mhw-dlc-20260922/`，截图/部分会话位于 `build/validation/mhr-device-20260922/runs/mhw-*`。存档基线备份在 before-savedata.tar；本轮不调用存档格式化。最终存档/配置比较待设备归还。完整 APK/host/JNI、测试及清理状态见 [manifest](mhw-dlc-silent-20260922/manifest.json)，游戏/固件二进制和存档仅留本地 build 目录，不入 Git。
