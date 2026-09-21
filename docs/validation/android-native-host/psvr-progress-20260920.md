# AYN 三款 PSVR：字符设备、内存布局与 VideoOut 事件修复

2026-09-20；`feature/malos/beat_saber_fix`，基线 `63b6d560`，本地未提交。延续[上一轮三款盘点](psvr-triage-20260920.md)，本轮在 AYN Thor `9c2841a4` 使用原有 ZAR，未重打包或替换游戏文件。

已部署新的原生实现；Sports 越过随机设备和 VideoOut 事件错误，Tetris 越过直接内存分配、高地址映射及三次内存池预留。两者仍在新的具名未实现接口处停止。Beat Saber 保持可见背景/安全提示/光剑，但双眼重影仍未修复，Continue 未验收。三款均不声明可玩。

## Sports：修复空 FILE 的来源，补 VideoOut 事件族

旧故障在 libc `fread+0x54` 读取 VA1。对照该游戏 libc 精确 ELF，指令访问 `FILE + 1`；开启本轮默认关闭的 `debug.shadps4.storage_log` 后，PID1652 明确先出现 `/dev/urandom` 打开失败 `fd=-1,error=2`，随后同一故障。不是 ZAR 丢失游戏文件。

GuestStorage 新增受控字符设备：`/dev/random`、`/dev/urandom`、`/dev/srandom` 用 host 系统随机源 `arc4random_buf`；`/dev/zero` 返回零，`/dev/null` 读取 EOF、写入丢弃。实现独立 guest FD、访问模式、Stat/Fstat、关闭、零长度、取消；设备不允许 mmap/截断/目录读取，positioned I/O 返回 ESPIPE，随机设备写入返回 ENOTSUP。只接受精确路径，不开放任意 host `/dev`。未复用桌面每次打开重新 srand/rand 的弱随机实现。

修复后 PID8051 和最终 PID28382 均打开随机设备成功、载入 Unity 资源。随后发现 VideoOut 事件 getter/删除缺口；同时盘点 Tetris 的 Vblank/事件 ID 需求，批量接入 GetEventId/GetEventData/GetEventCount、AddVblank（含 Sys 别名）、DeleteFlip/DeleteVblank。getter 先复制经过验证的 guest wire event，再调用桌面共用解码，不把 guest 指针传给 native；输出验证失败保留明确错误。Android 注册/删除受 port 锁保护，队列已有生命周期保留到 GPU worker join。

同时修正桌面共用事件实现：删除使用实际 Flip/Vblank ident 而不是显示 handle，重复删除返回 INVALID_EVENT；Flip 的 48-bit 负参数恢复符号扩展；事件计数使用 bit_cast 读取位域，原 static_cast 实际只初始化首字段。首轮测试的六处失败保留于证据（四处计数断言也修正为协议中的高四位，不再将整个低16位当计数）。一次设备清单还揭示 getter 虽有 handler 却未加入准入集合，已补齐；最终完整清单证实 GetEventData 实际 android_bridge。

最终 Sports PID28382/gen1 越过上述两处，读到 `globalgamemanagers.assets` 并创建音频流，随后在 libc `_is_signal_return`（`crb5j7mkk1c`）具名拒绝退出。未出现此前 fread SIGSEGV；没有菜单或玩法画面验收。下一族是 guest 异常展开/模块信息：`sceSysmoduleGetModuleInfoForUnwind`、`sceKernelGetModuleInfoFromAddr`、`sceKernelInternalMemoryGetModuleSegmentInfo`、signal/debug/backtrace 等均保留全清单，不将 `_is_signal_return` 简单改为固定0。

## Tetris：直接内存族与实际地址布局

新增九项检查桥接：AllocateMainDirectMemory、AvailableDirectMemorySize、CheckedReleaseDirectMemory、MemoryPoolExpand/Reserve/Commit/Decommit/Batch/GetBlockStats。使用桌面 MemoryManager；复制 guest 输入、验证输出映射身份，跨 mapping writer 不持 data pin。Batch 先复制完整 entries，再执行，保留部分完成计数；Move 返回明确 ENOSYS，未声明任意移动可用。GetBlockStats 延用桌面统计语义，未声明完整物理内存精确统计。

共享 MemoryManager 修正物理搜索起点/终点及对齐、溢出/无效范围；PoolExpand 不越过 searchEnd；exact-budget Commit 可用，Commit 应用实际 CPU protection；Decommit 验证完整覆盖范围而不是仅首块，保留映射退休与 GPU/host 生命周期协议。

实测发现两层地址冲突：

1. 游戏在 `0x4000000000`（256 GiB VA）映射 512 MiB，旧 runtime envelope 只有120 GiB，故 MapDirectMemory 返回 EINVAL。
2. 它随后在64 GiB、128 GiB等固定地址预留 pool（已捕获的两次请求长度均8 GiB）；旧 runtime 将主线程栈放在64 GiB，Reserve 的输出也在待覆盖区内，正确触发保护。未取消该保护，而是将 runtime 栈/TLS/服务分配起点统一移到112 GiB。

最终保留 guest 的低地址小段和4–288 GiB段，仍从 `/proc/self/maps` 排除实际 host 映射并用精确 advisory mmap 验证；FEX embedder 策略上限512 GiB，return gate 留在 envelope 外。早期510 GiB连续保留挤掉12 GiB host backing 的可用空间，报 ENOMEM，已收回；曾仅追加256–288 GiB的中间版又漏了128 GiB pool，均保留为反例。

GPU 页状态改为按需1 GiB地址分块（每块256 KiB状态字），不随整个 VA 上限预先触碰128 MiB表。fault 端仅固定层级原子查表，不分配、不加锁；块在会话所有 fault owner 退出后随 runtime 释放。两处相距256 GiB的映射只需两个块的测试通过；这不是整游戏 RSS/PSS 节省测量。

最终 Tetris PID27309/gen1：AllocateMain、MapDirect、AvailableSize、PoolExpand 和三次 PoolReserve 返回0；随后在 `sceCoredumpRegisterCoredumpHandler`（`8zLSfEfW5AU#libSceCoredump#1#libkernel#Function`）拒绝。此处是新回调族边界，未伪造注册成功。仍未完成 libc DT_INIT/游戏启动，更没有 EOS/Wwise/可玩声明。

## Beat Saber：重影定位收窄

复用基线 RDC 身份 SHA `5bfe8050…`。本轮通过 AYN 远端 review_texture 实际查看 event1486/resource1535 的 RGB：原始 guest 眼图已经有重叠文字和多组光剑，故不能只归因 host SBS 后处理。Alpha0 的旧问题已由上一轮修复，本轮未再次改 Alpha 或复制图像伪装双眼。

配套 RenderDoc 的结构化 XML 导出（不在 macOS 本地回放 Adreno）进一步给出：event1475/1486 的最终两次 guest 绘制分别设置 `x=0,width=672` 与 `x=672,width=672`、高度756的 viewport/scissor，输出1344×756图1535；Quad patchControlPoints=4。两次都采样1985和1851；1851来自1825的两次复制/resolve类绘制。下一步应沿1825→1851→1535读取实际眼图、采样坐标和每眼常量，尚无唯一 shader 根因。

离线 deep_pipeline 仍 pending，实时 pipeline_bindings/cbuffer_get 返回 native-abi-required；没有把空数据当“无异常”。结构化导出只提供实际 Vulkan 状态，不代替尚未取得的上游像素证据。[渲染选择器、来源和局限](psvr-progress-20260920/render-evidence.json)。

1.0 诊断 PID22641 仍能复现同类重影；最终0.5的 PID29579 也复现。两版渲染代码未改，但 APK 不同，属于“1.0同样复现”的排除证据，不是严格同APK性能A/B。1.0临时设置已恢复，未作FPS改善结论。陀螺仪继续启用、每次会话初始正前方；未新增受控物理转动验收。NP ENOTINIT、LoginDialog provider、Move指向和Continue仍待办。

## 验证和交付

- 最终 host 在 AYN：文件I/O **501/0**、真实 MemoryManager **106/0**、graphics admission/事件读取/稀疏页状态 **48/0**；日志见 [final-host-tests.txt](psvr-progress-20260920/final-host-tests.txt)。
- 真FEX高地址 **3/0**，覆盖100轮低地址/256 GiB间隔代码执行、退休后重发高地址代码、owner销毁。不是所有48-bit地址或16 KiB host页支持声明。
- 最终 APK `01b73f37…`、host `08c98bba…`、JNI `382eebb6…`，已安装，三者本地包内容与设备安装文件SHA匹配；[完整身份](psvr-progress-20260920/final-binaries.json)。
- 保存实际全量绑定和按库的拒绝族，未将静态NID存在当成功：

| 游戏 | 完整行数 | guest_export | runtime_bound | refused行/唯一 | not_relocated |
|---|---:|---:|---:|---:|---:|
| Beat Saber | 1939 | 780 | 792 | 353 / 328 | 14 |
| Sports | 2303 | 969 | 913 | 410 / 361 | 11 |
| Tetris | 2603 | 1146 | 915 | 525 / 403 | 17 |

拒绝项仍含在线服务、音视频、平台、回调、卸载等完整族；并非全部不阻塞游戏。Tetris 的 WindowModeMargins 在桌面也只是 stub，本轮没有借此准入假成功。

保留此前所有本地修改，本轮未 commit/push；未做全游戏回归、GPU全套重跑、整局可操作性、总RAM或性能收益声明。后续顺序：先补两个新游戏共同需要的异常/模块元数据与受控回调合同，同时继续 Beat 的真实上游眼图定位，避免逐个入口零返回。

最终 PID29579/gen1 在5,376次 host presents 后正常 UI Stop，`Stopped/user_stop`，guest return=2147614724（不是0；managed取消exitCode=0属于不同字段）。token/buttons=0、RenderDoc API未加载、TracerPid=0；临时storage log和replay loader属性已清空，全局配置按原文件恢复0.5/High，Turnip/SBS保留。详见证据目录的 final-status/input/renderdoc/process-status。
