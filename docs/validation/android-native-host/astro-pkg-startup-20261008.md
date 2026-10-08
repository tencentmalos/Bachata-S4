# ASTRO BOT 1.04：Android PKG 启动修复（2026-10-08）

## 环境与范围

- AYN Thor，序列号 `9c2841a4`，Android API 33，Adreno 740。
- 分支 `feature/online_shop`，已快进合入 `origin/malos/main` 的 `23d1baba1f4361eb56cf7ac263681934228ee04d`。本轮改动尚未提交；工作区包含其他任务的改动。
- CUSA12392 本体 + 1.04 更新直接挂载，设备路径 `/sdcard/game/ps4/roms/CUSA12392.pkg` 和 `CUSA12392-UPD.pkg`。两包完整 SHA-256 与本机原件一致。
- 保留用户 Turnip：`01a3548fbd2695f3b27ad896bfbbbf0561e28c49ad1de8a19b05745acd01ba19`；SBS、0.5 内部渲染比例、FSR1。不是 Swan/OpenXR 头显验证。
- 本地完整日志、构建记录、每阶段导入清单、设置/存档备份在 `build/validation/merge-thor-20261007/`。游戏分析文件留在仓库外 `/Users/bytedance/game/ps4/astra/analysis-20261008/`。

## 启动依赖与错误处理

实际加载模块的清单为 `astro-imports-*.json`，记录 importer、完整 NID、函数名、guest 地址、provider 和绑定状态；不是只按源码里存在函数判断可用。麦克风修复前清单共 1216 行，包含 88 行明确未支持导入和 141 行未登记状态，不能据此宣称所有库均已实现。

| 功能族 | 观察与处理 |
| --- | --- |
| pthread 属性 | 游戏复用栈输出槽，原 Init 读取旧值并返回 EBUSY。初始化改为创建新的默认属性，不依赖输出槽原内容。 |
| GNM / 内核事件 | 补已有桌面的 WaitFreeSubmit 别名；实现 Android HR timer 截止时间和队列等待，关闭队列可取消等待。 |
| 登录 / 系统服务 | 按核实 ABI 补登录初始化及对话框生命周期；平台隐私查询返回明确不可用，不用未写输出的成功桩。设备请求/对话框结果等未实现能力仍拒绝。 |
| 图片 / 字体 | 挂载用户本地固件 JPEG、PNG、Font、FontFt 及 FreeType 依赖，校验库身份和允许目录。只对三个已证实可空的可选 bitmap driver 数据符号返回空；不提供替代字体文件。 |
| Companion / 录制 / 截图 | Companion 复用桌面离线状态与事件语义，不启动传输、不调用 guest 指针形式的 native 回调。录制明确返回 UNSUPPORTED；截图开关/叠加参数为会话状态，实际截图服务仍不可用；离线 SharePlay 禁止操作可成功。 |
| 手柄音频 | 原 Android 对 PadSpk 一律 NOT_OPENED，使游戏 Sound.cpp:925 断言。现与桌面一样通过宿主音频输出，独立队列/句柄、最多四路、实际应用 mix level；输出状态报告宿主 PRIMARY，不声称存在物理 DS4 扬声器。 |
| 麦克风 | 原 NullAudioIn 打开失败，游戏将随后 Input 错误值当采样数，传出 `memcpy` 长度 `0xfffffffe00980404`，在 libc+0x18bb6 崩溃。新增会话内无设备模式，对齐桌面断开麦克风时的静音流，返回 DEVICE_NONE；读取经 guest 地址校验并按块周期节拍，关闭/停止取消等待。没有开启 Android 录音，也没有真实麦克风/吹气玩法支持。 |

用户提供的固件只部署到设备 `files/host/sys_modules`；源文件、游戏二进制均不进入 Git。剩余可选接口保留明确拒绝，后续若实际阻塞再按对应功能族分析。

## 首帧等待的证据与修复

此前无断言但一直 0 帧。Guest 主线程等 `0x300017b70` 的 mutex，持有者 Guest-21 正等待 VR 帧完成。GPU 标签数组 `0x30e318e80` 已写出 `{0,1,2,0}`，而游戏 END 事件计数 `0x3317850` 始终为 0。

游戏只登记 Reprojection END user event（队列 `0x40000008`、id 3），在 END 到来后才提交第一帧。旧实现只有提交帧 GPU 退休才发送 END，因此互相等待。游戏还会在初始化过程中调用 ReprojectionStop；仅增加未提交时的 END、却继续以 stopped 屏蔽周期的第一版修复仍然无效。

最终显示周期由 initialized + display 决定：首次提交前、重复显示以及 Stop 后，没有在途眼图读取时发送 END；存在 GPU 读取时仍由退休回调发送完成，避免提前报告读取结束。UnsetDisplayBuffers / Finalize 关闭周期。Stop 继续等待已提交读取退休。设备随后开始连续执行 `dntZTJ7meIU`，双眼 1440×1536（宿主 720×768）进入 SBS 1920×1080 合成，验证等待链已解除。

Guest RSP 证据是 HLE 边界的已保存寄存器，不是异步 JIT 寄存器猜测。两次 guest 调试会话均已清理、forward 已移除、TracerPid=0。Native LLDB 启动受工具链版本检查拒绝，未创建会话；没有用它作为运行栈证据。

后续冷启动还捕获到 `HmdGraphics.cpp:500 / sceHmdReprojectionStart 0x81110011`：两个宿主读租约尚未退休时，第三次有效提交被内部容量限制拒绝。改为有界、可取消地等待真实 GPU 退休后接收；继续最多两个在途读租约，不通过加大队列或提前发完成规避。2 秒超时、会话取消、Stop/Finalize 仍返回错误。对应测试增加等待、退休后复用槽位和取消覆盖。后续实测又定位到 VideoOut 呈现队列同样限制为两帧：眼图读取完成不代表窗口已经呈现完成。该队列也改为等实际 flip 完成或取消后再接收；关闭端口、停止、丢弃/完成 flip 均唤醒等待。

## 进入标题后的宿主崩溃

01:39:15 捕获到 native 断言 `recorded command payload 221336 too large`，宿主 Build ID `2b49fc592d5103f08c2771adb02916fa6580c68c`。调用链为 `Image::UploadRegions → RecordingCommandBuffer::pipelineBarrier2 → CommandRecorder::EnsureRoom`：一次上传产生的屏障数组超过固定 128 KiB 块，换一个同样大小的空块后仍然放不下。

`CommandChunk` 现在只在空且尚未分配 payload 时按需扩大存储；已经录入的指针不移动。保持异步录制及 pre-pass/held-pass 顺序，不拆散屏障、不回退到越序的直接调用；Reset 按实际容量复用。容量计算在分配前检查整数溢出。Android 测试通过真实录制器加假的 Vulkan dispatch 验证 2048 个图像屏障、内存/缓冲屏障深拷贝、源数组销毁、普通及前置/持有通道排序、工作线程和池复用。

## 针对性验证

| 检查 | 结果 |
| --- | --- |
| Thread attributes | macOS 65/0，Android 65/0；旧实现负对照 31 项失败（含级联）。 |
| Login | macOS、Android 各 25/0。 |
| Kernel app info | macOS、Android 各 19/0。 |
| Equeue | Android 63/0。 |
| Process services | Android 1765/0（无设备模式接入前的版本）。 |
| Capture services | Android 147/0。 |
| Reprojection | Android 102/0，涵盖空闲周期、Stop/Unset/Finalize、在途 GPU 读取不提前通知、队列等待/退休/取消。macOS portable 目标缺 JSON include 及 sensor/log 链接依赖，未运行。 |
| Audio | Android 179/0，含 PadSpk 实际 PCM、默认/自定义增益、关闭与容量，麦克风静音字节范围、边界无效指针、无设备状态、节拍与停止取消。旧队列测试未隔离新 limiter，已显式关闭该测试的 limiter，保留原饱和输出期望；生产 limiter 设置未改。 |
| Command recorder | Android 11/0，包括 221336 字节 payload、溢出拒绝和工作线程实际执行的超大 `pipelineBarrier2`。 |

## 持续启动与未解决项

- 容量修复包 `0470d8fe…` / host Build ID `5ce2233a5db5567c821f66a6bc039e03f514afab` 的 PID 6549 连续运行超过 9 分钟，16,213 次 frame 通知，watchdog stalls/dumps 均为 0；日志没有新的 guest 断言、GUEST_FAULT 或容量断言。一次有界 Cross 输入后越过标题到就坐校准页。帧通知不是对画面或游戏逻辑正确性的验收。
- 后续诊断包 PID 7963 冷启动仍可到标题，超过 6,700 次 frame 通知未再次触发启动断言；诊断只加日志，不改变功能。不要把这轮记为最终 APK 的完整回归。
- **渲染未修好**：标题有大片黑色几何遮挡和三角破面。校准页同会话 recorder off / on、hoist off / on 的截图中异常有变化，期间管线跳过计数为 0，但不足以证明缺失的 GPU 依赖。只禁止 attachment barrier 提前执行的实验仍有破面，已撤回；`image.cpp` 最终无改动，没有将关闭优化写成默认或每游戏配置。
- 保留了一帧 PM4/动作 trace `bd41d44fac3124ef8747513d80b9c04a`，flip 2307→2308，5 个提交、153 个动作、85 个 host 事件，结构校验通过，无截断。它不是确定性 GPU replay，也没有记录所有资源字节，不能据此宣称已定位错误像素的生产者。文件在本地验证目录 `astro-hoist.{gpu.pm4.trace,gcmdtrace.ps4}`。
- **校准/玩法未通过**：当前 Thor 的 SBS 模式没有 DS4 空间位姿来源，VrTracker 对已连接但未追踪的 pad 返回 NOT_TRACKING；没有伪造追踪成功来跳过校准。没有进入关卡、验证存档或吹气玩法。

最终重新安装的 APK 与九分钟验证包逐字节相同（完整 SHA-256 `0470d8fec7d3c3d732a0c8661275198bcaec8f28f5a811373e227337cee07987`），已核对安装件和宿主 Build ID。[身份与测试证据](evidence/astro-pkg-20261008/build-provenance-current.json)以及同期截图保留在 `evidence/astro-pkg-20261008/`。最后冷启动 PID 9562，重新运行录制器测试 11/0，880 次帧通知时没有新断言；过程状态在本地 `deployment-status.json`。

保留用户驱动、PKG、固件、存档与配置。诊断捕获和有界输入均已停止，无后台测试脚本、Guest RSP 或 native LLDB 会话。上述单测和连续提交不构成可玩性、音频听感或头显画面验收。
