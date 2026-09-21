# PSVR 参考续修与 AYN 三款复测（2026-09-21）

本轮补齐空目录删除、VideoOut 可选参数 ABI、HMD 双眼偏移输出，并修正共享 Move 输入时间戳单位。最终 APK 已安装 AYN Thor `9c2841a4` 并核对完整 SHA。Tetris 越过前两处 VR 接口阻塞后仍崩溃；Beat Saber 和 AllInOneSports 能显示安全提示，但显示与交互仍未完成，三款均未达到可玩验收。

分支 `feature/malos/beat_saber_fix`，基线 `63b6d560dd43af031ec7a5de64ae6204beea5de2`，保留既有修改，未 commit/push。前一增量的显示安全区查询见 [单独报告](tetris-safe-area-20260921.md)。本报告中的 V2–V5 是此次增量的构建标签，不对应之前报告的同名标签。

## 参考资料与采用范围

用户确认“远端新增参考”就是所附 `PS4_Unity_PSVR_Analysis.md` 和 `libpsvr-0.1.zip`。已读取两份资料，身份见 [reference-identity.json](psvr-references-20260921/reference-identity.json)。`git fetch origin` 未得到新增提交。本轮没有等待另一个未指明的远端仓库。

分析文档给出的 Unity/Morpheus 初始化、render-thread、tracker 和 pose 路径用于核对当前 Beat 1.00 的 Unity 2018.1.9f1 ELF；2.04 的 Unity 2022 偏移不能套用到当前版本。Reverse Study 检查了 pose 更新 `+0xd6e0a0`，分析 ELF 仅修改 e_type，源码内容未改写。最初取到 `+0xd6c400` 的结果已识别为不同函数，未据此改动生产代码。工作区已关闭，没有发布新的重建符号。

libpsvr ZIP 是 11 个文件的 USB/HID 参考，含头文件及小型 `.lib`，没有 `.cpp` 实现；不能直接充当 Orbis 的 HMD/Reprojection provider。本轮未链接该二进制。设备状态、佩戴状态及 IMU 时间信息只作为协议参考；显示参数的具体 ABI 另与本机固件 11.00 模块核对。[VideoOut 身份](psvr-references-20260921/videoout-identity.json)、[HMD 身份](psvr-references-20260921/hmd-identity.json)。没有复制完整固件/游戏二进制进证据目录。

## 实现

### 空目录删除

在 [GuestStorage](../../../src/core/host_runtime/guest_storage.cpp) 加入 `Rmdir`，kernel/posix NID 共用路径复制、慢调用、errno 映射和现有挂载策略。使用目录 fd 相对的 `unlinkat(..., AT_REMOVEDIR)`，只删除一个空目录；拒绝挂载根、`..`、只读内容、受保护的 savedata 路径、文件与叶子符号链接。末尾 `/.` 返回 EINVAL，空路径返回 ENOENT。namespace/quota 锁与现有目录操作保持一致，不递归删除。Runtime 的 socket-fd 分流明确排除这个路径操作。

### VideoOut 可选参数 ABI

真实 Guest 断点捕获 Tetris 对 `sceVideoOutConfigureOutputMode_` 的调用：handle=1、reserved=0、mode_size=32、options=NULL、options_size=16；mode 的 size=32，其余字段均为 Any。旧桥接错误地要求 NULL options 必须对应 size=0，返回 `0x80290001`。游戏随后进入 fatal 文本路径，旧 VA8 崩溃是错误处理过程中的二次故障。

固件 `+0xd780` 校验接受 options_size 0 或 16，与指针是否为空无关；`+0xdc19` 在指针非空时读取 16 字节，否则提供零默认值。[校验指令](psvr-references-20260921/video-mode-validator.asm)、[可选参数处理](psvr-references-20260921/video-mode-options.asm)、[真实参数与原始报错摘要](psvr-references-20260921/diagnostic-summary.json)。

新 [ReadGuestVideoMode](../../../src/core/host_runtime/guest_video_mode.h) 按此读取，保留完整可读区校验、只读 Guest 输入支持和失败时不发布半份请求；只有本地副本传给 native provider。原 provider 对 Any/HDR 的能力边界不变，不宣称完整显示模式支持。V3 实际越过该调用后到达下一项 HMD 导入拒绝。

两次 Guest RSP 会话分别捕获原始 fatal 文本及模式调用参数，清理均返回 ownershipReleased=true。协议没有提供 host PID/starttime、main Build-ID，故这里只声明外部 PID/status 与指令字节核对，不声明完整运行身份自动绑定。原始 RSP 结果在证据目录保留。

### HMD 双眼偏移输出

[GuestHmdEyeOffsets](../../../src/core/host_runtime/guest_hmd_geometry.h) 准入并绑定 `sceHmdGet2DEyeOffset`，复用现有 SBS native provider 的初始化、handle 与 63 mm 眼距策略。先用本地结构取得结果，再同时 pin 两个可写输出；任一输出失败时两边均不写入。

固件实际只写每个输出的三个 float（12 字节），并不覆盖结构尾部 20 字节 reserve。[指令证据](psvr-references-20260921/hmd-eye-offset.asm)。生产桥接因此只 pin/copy 12 字节，保留调用方 reserved，支持恰好位于映射末端的有效输出。最初草稿按整个 32 字节处理的问题已在最终测试前修正。没有改变投影、头部姿态或重投影算法。

### 共享 Move 时间戳

OrbisPadAdapter 生成微秒时间戳，JNI 原样传入 MoveInputSnapshot；旧 Move provider 将该数值当作纳秒再除以 1000。现在 [MoveInputSnapshot](../../../src/core/host_runtime/guest_vr_sensor.h) 明确命名 `timestamp_us`，[Move provider](../../../src/core/libraries/move/move.cpp) 原样输出。头部陀螺仪的 `timestamp_ns` 仍使用纳秒，未改变。

测试覆盖相邻 1 微秒样本及较大时钟数值，通过真实 Move Init/Open/Latest/Recent/Term 路径读取。Recent 的完整多样本历史、Move 射线命中及 Continue 行为没有在本轮补齐；不能把时间单位修正称为输入适配完成。

## 构建与定向验证

最终 native 构建、Gradle APK 构建通过。AYN 上执行并链接最终 APK 同 SHA 的 Host 库：

| 测试 | checks | failures |
| --- | ---: | ---: |
| Guest file I/O，含空目录删除/errno/权限/符号链接/有效打开 fd | 530 | 0 |
| Graphics admission，含四种 options 指针/size 组合及内存边界 | 73 | 0 |
| PSVR services，含安全区、眼偏移双输出与 Move 时间戳 | 217 | 0 |
| VR sensor，含明确微秒存储 | 37 | 0 |
| 合计 | 857 | 0 |

这是相关定向测试合计，包括此前已有检查，不是 857 项全新测试或完整游戏回归。原始输出分别为 [files](psvr-references-20260921/files-v5.txt)、[graphics](psvr-references-20260921/graphics-v5.txt)、[PSVR](psvr-references-20260921/psvr-v5.txt)、[sensor](psvr-references-20260921/sensor-v5.txt)。首次构建使用不存在的 target、一次 CHECK 宏中逗号导致编译失败，均已修正，保留失败日志；首次 sensor 执行漏配 LD_LIBRARY_PATH，正确设置后通过。不能把中间尝试写成全部成功。

最终 V5：APK `2470f50e8d4aabea6abee0ca2296472c61640c69e2818906c6cd2558af50aada`，Host `2727473d7a2f3e5e47e2c4f73bdc242f457193fc3280d245b893e4a043fc8f06`，JNI `6f7d9f27d1ac8a35377d578fc9bae8add4c111f73c2e841f9329a961704ff668`。安装后拉回整个 APK，SHA 完全一致。[构建身份](psvr-references-20260921/build-identity-v5.json)、[源码身份](psvr-references-20260921/source-identity.json)。源码身份覆盖完整文件，包含此前本地修改，不把文件中所有改动都归因于这一轮。

## 三款最终实际效果

均使用原有内容、Turnip、Render 0.5、Texture High、SBS/gyro；没有为测试切换游戏驱动或倍率。下表是启动验收，不是性能 A/B。

| 游戏与最终进程 | 实际结果 | 终止状态 |
| --- | --- | --- |
| Beat Saber，PID21305/gen1，run `fd9bf0f7b1bf3801a330c84dbb7f33ca` | 正向安全提示与光剑可见，明显双眼重影；触屏 R2/Cross 尝试后仍未过 Continue，没有证明这次触屏输入到 Guest 的完整链路 | 2767 presents 后 UI Stop；Stopped/user_stop，guest return2147614724，非 0 |
| AllInOneSports，PID16974/gen1，run `8a061e9bb83e1886e81d11d1bcd8ed20` | 安全提示倒置、重复并偏到左侧；未进入项目选择或玩法 | 6292 presents 后 UI Stop；Stopped/user_stop，guest return4，非 0 |
| Tetris Effect: Connected，PID20364/gen1 | 越过此前 VideoOut 参数错误及 Get2DEyeOffset 拒绝；约 8 秒后 Guest-1 在空地址崩溃，没有正常游戏场景 | SIGSEGV/VA0，mapped RIP `0x4051e95`，与 V4 PID11839 相同；未取得崩溃前 run UUID |

[Beat 实景](psvr-references-20260921/beat-v5b-after-input.png)、[Beat 终态](psvr-references-20260921/beat-v5b-stopped.txt)、[Sports 实景](psvr-references-20260921/sports-v5-adb.png)、[Sports 终态](psvr-references-20260921/sports-v5-stopped.txt)、[Tetris 故障](psvr-references-20260921/tetris-v5-fault.txt)、[本应用日志](psvr-references-20260921/logcat-app.txt)。

一次隐藏 scrcpy renderer 截图仍显示之前的 Library/上一款画面，与活动进程不符，该截图不作为最终实景证据。最终采用 adb 原始屏幕截图，Beat 又以同一 V5 重启复核；上表使用新 PID21305，不把 PID15922 或 V4 的画面冒充本次截图。旧 PID15922 也正常 Stop，保留终态记录。停止后旧游戏界面残留的问题仍存在；确认 session:none 后才重开 Library。

Tetris 的 mapped RIP 对应主模块 `+0x3c51e95`：`cmpl %eax,(%rcx,%rsi)`；附近从对象 `+0xd80` 读取表数据指针，再按索引访问链。[局部反汇编](psvr-references-20260921/tetris-v4-fault.asm)。目前只有该访问和 VA0 的证据，尚未证明是分配失败、未初始化、内存损坏或 HMD 输出引起；没有添加跳过判空的 Guest patch。反汇编开头由范围截断产生的无效指令不能作为函数入口证据。

完整导入/绑定及按库归族清单均保留，不能把绑定成功等同语义完整：

| 最终 V5 | 总行数 | guest_export | runtime_bound | refused 行 | not_relocated | 唯一 refused |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [Beat 导入](psvr-references-20260921/beat-v5b-imports.json) / [按族](psvr-references-20260921/beat-v5b-audit.json) | 1939 | 780 | 867 | 278 | 14 | 265 |
| [Sports 导入](psvr-references-20260921/sports-v5-imports.json) / [按族](psvr-references-20260921/sports-v5-audit.json) | 2303 | 969 | 1004 | 319 | 11 | 290 |
| [Tetris 导入](psvr-references-20260921/tetris-v5-imports.json) / [按族](psvr-references-20260921/tetris-v5-audit.json) | 2603 | 1146 | 1022 | 418 | 17 | 329 |

## 下一步的明确边界

1. **Tetris 空地址访问：** 对 `+0x3c51e95` 前驱和对象 `+0xd80/+0xdb8/+0xdc0/+0xdc8` 建立真实调用/分配/初始化证据，检查首次不满足容器不变量的位置。当前不能将其继续归为已修复的 VideoOut fatal 路径。Commerce 离线合同以及 legacy Reprojection Start/Start2dVr/WithOverlay 整族仍未完成，不能准入桌面 stub 后直接返回成功。
2. **Beat/Sports 双眼与 Move 交互：** 在同一 APK/驱动/场景关联 Guest 原始眼图、array slice/atlas UV、最后 Guest draw 和 Host presentation；逐层验证眼图表示、Y 方向、layer/overlay 与姿态矩阵的所有权。现有桥接仍有未消费的 layer/pose 参数，本轮没有新的 RDC 证明它们就是唯一根因。以实际 Move 射线/按钮消费者证明 Continue 命中，不用更多单位四元数或零返回遮盖问题。
3. **UI 生命周期：** Stopped 后应退出旧呈现页，当前仍需确认 session:none 后重开 Library。未在本轮改变此流程。

没有全三款可玩、完整 SBS/PSVR/桌面合同、全回归或性能提升结论；此前资源策略 spec 的未验收项不因本轮通过而关闭。

## 交付与清理

最终为 Library/PID22493、session:none、TracerPid0、无活动游戏服务；两次 RSP 调试器已释放所有权，debug port/wait=0、无 adb forward、自有 scrcpy 正常退出0、logcat 采集停止。global/config 与本轮开始前逐字节一致，保留 Turnip/0.5/High/SBS/gyro，初始正前方策略未改动。[交付记录](psvr-references-20260921/delivery.json)、[Library](psvr-references-20260921/library-final.png)、[证据 manifest](psvr-references-20260921/manifest.json)。
