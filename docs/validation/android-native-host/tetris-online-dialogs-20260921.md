# Tetris / OnlineSubsystem、对话框及 PSVR 启动续修（2026-09-21）

本轮针对 AYN Thor `9c2841a4` 上的 Tetris Effect: Connected（CUSA13427）继续对照桌面流程。修复了两处“provider 加载失败后留下半初始化对象”的实际空指针链，并补齐所涉及的本地对话框、离线 NP 对象入口和实体键盘监听。已有 `GuestImeDialog` 输入对话框保留；此次增加的是另一组 `libSceIme` Keyboard 入口，不是重新实现 IME 输入框。

源树为 `feature/malos/beat_saber_fix`，基线 `63b6d560dd43af031ec7a5de64ae6204beea5de2` 加全部现有未提交工作。本轮未 commit/push，没有进行完整游戏回归。所有文件与 SHA 见同名证据目录的 `manifest.json`；其中 build/status/logcat 是各验证时点的记录。

## 已证实的两条失败链

1. 原版运行 PID 22493：VR manager 初始化中，`LoadModule(0xa4 / libSceMsgDialog)` 返回 `0x805a10ff`，提前返回，未构造对象 `+0xd80/+0xdd0` 的表。此前 Tracker 初始化结果却已保存为成功，后续 `main+0x3c51e95` 按这个结果继续注册并解引用空表。Guest RSP 读取实际对象字段、返回值和代码字节，支持“缺 MsgDialog provider → 半初始化 → VA0”的因果链；不是位姿或分配器结论。
2. 补 MsgDialog 后，PID 2702/9022 进入另一个空指针：`main+0xe190ad` 调用 OnlineSubsystem，实际虚表槽 `+0x58` 指向 `main+0x4f80c0`，返回 NULL。静态初始化链 `main+0x5d913d` 的 Commerce 模块 `0xa8` 加载失败会返回 false，尚未创建后续对象。RSP PID 3904 捕获调用前与返回后的值。新增 Commerce provider 后，运行越过这处崩溃，进入 Score/TUS、键盘和 HMD 初始化。

RSP 的主模块基址为 `0x400000`。归一化分析 ELF SHA `b8da0a334a866e5d124488d23ff3ae66a187cb731e3f80626e549a3f98de70b6`，原导出 ELF SHA `157845ba4297a000ac9cfeb26b7b78925d7d903a2ee34575c74d0f5ede6e9677`；归一化仅改变 ELF 类型/分析基址，PT_LOAD 内容不变。调试器未提供完整 host PID/start-ticks/主模块 Build-ID 身份握手，使用独立设备状态、基址及现场代码字节核对，不能提升为完整身份验证。Reverse Study 对 `+0xe19040/+0x4f80c0` 返回 NOT_IN_FUNCTION，不代表函数不存在或没有调用者。两个临时 study workspace 已关闭。

## 本批合同与桌面复用

| 族 | 实现范围 | 明确边界 |
|---|---|---|
| Common/MsgDialog | 每会话 CommonDialog 所有权；10 个入口、检查参数/嵌套字符串、状态/结果、进度与取消；复用现有 ImGui 渲染路径 | user/system/progress 支持；无猜测未知模式。旧 SDK 文本差异未完成；未观察游戏实际 Open |
| CommerceDialog | 11 个入口，商店图标复用桌面 PsStoreIconLayer；共享 CommonDialog 租约；本地不可用说明框，检查 128 字节参数与 48 字节结果 | 用户关闭为取消，授权 false；没有商店登录或购买成功。SDK<1.7 及内部模式 1000/1001 不准入 |
| NP Score | 8 个入口：现代 CreateA 采用桌面 SignedOut 顺序；请求/标题生命周期与 Poll/Wait 明确 InvalidId，失败不改输出 | 不创建在线上下文、不返回异步假完成；在线排名/旧身份创建/桌面 stub 不准入 |
| NP TUS/TSS | 8 个入口：复用 Offline::Identity 的 SDK 分支，现代离线创建和无效句柄生命周期 | 在线数据/变量、旧身份入口不准入 |
| IME Keyboard | Open/Close/GetResourceId/GetInfo/Update 五入口，检查参数与可执行回调，拥有参数快照，Stop 清理 | 没有实体键盘 provider：资源枚举为空并返回未连接，GetInfo 返回无资源；没有伪造键盘事件，不将 guest 回调当 native 函数调用。原 ImeDialog 不变 |
| ErrorDialog | Init/Open/Close/Term/Status/Update 六入口，16 字节参数安全复制，显示实际错误码 | 保持桌面独立生命周期；OpenDetail/OpenWithReport 仍拒绝；游戏只观察到轮询，模态效果由真实 ImGui 定向帧验证 |
| NP WebAPI | 在既有 7 项本地控制对象上接入 void CheckTimeout，复用桌面实际超时扫描，桥接保留 RAX；全局节流时间移入已有互斥区 | 不是在线请求完成，不传 guest 指针；用户上下文、HTTP 请求、push callback 仍待独立桥接 |

MsgDialog 的输出一次写入完整 44 字节结果，回调/响应以 request generation 防止旧响应误入新请求。进度 Inc 的 u32 溢出后 min(100)、SetValue 保留原值而 UI clamp，已与本机 firmware 11.00 指令核对。Close/Terminate/Stop 释放输入占用，打开时已有按键不会直接确认。ErrorDialog 覆盖已有消息框后，渲染器通过弱引用栈恢复底层 Running 对话框，避免遗失 UI 及输入占用；锁顺序不在 renderer 锁内获取 dialog 锁。

固件用于核实 ABI/返回行为，身份与有界反汇编位于证据目录；未提交游戏/固件 ELF、完整反汇编或 APK。

## HMD 工作区误判

V6 PID 25460 的 VA8 是 fatal 日志栈的二次失败。RSP PID 26745 捕获原始 UTF-16 错误：`sceHmdReprojectionInitialize` 的 BORDER_FOR_SINGLE 模式返回 `0x81110009`。另一轮 PID 28783 在初始化调用前读取真实 56 字节配置：onion=`0x3000021000`，garlic=`0x202fd00000`，selector=3/3、type=2。

Garlic 的 1 MiB 工作区覆盖两个相邻 rw 映射 `0x202fd00000–0x202fde0000` 和 `0x202fde0000–0x202fe00000`。原 AcquireDataBatch 把整个工作区当单映射请求，误判合法跨映射范围。修复按实际 Query 段拆分，并以每段 mapping generation 一次 AcquireDataBatch 验证；仍拒绝洞、权限不符和溢出，不放宽底层单映射 API，不持久 pin 工作区。相同 49 项测试，旧实现 6 项预期失败（含 Finalize 级联），新实现 49/0。

这里只修复工作区地址校验。尚未实现 single/2d/overlay Start 的完整提交合同或 native 光学重投影，不能视为 Tetris SBS 已完成。

## 验证

最终 V9 Host 在 AYN 原生执行：

| 测试 | 检查 / 失败 |
|---|---:|
| Reprojection（含相邻段/权限/洞/溢出） | 49 / 0 |
| NP（含桌面对照、SDK 边界、失败输出保持） | 698 / 0 |
| CommerceDialog | 178 / 0 |
| Msg/ErrorDialog（含真实 ImGui 帧与模态恢复） | 189 / 0 |
| PSVR services（含键盘、并发 WebAPI pump） | 289 / 0 |
| Host lifecycle smoke | 511 / 0 |
| 合计 | **1914 / 0** |

WebAPI pump 在初始化前、终止后以及两个并行线程各 100 次调用时运行，随后删除 filter/handle 验证对象仍有效。这里没有创建在线请求，不是网络超时传输验收。头文件声明缺失等中间构建失败也保留，不计入通过检查；修复后的 native 构建、APK 构建和 diff whitespace 检查通过。

V9 APK SHA `889a5792c04054eec62ff43361b5ea40de253d0fe698e17742673b06f50579f4`；Host `5bf11adee4716ce9022157e80b112b836ef9923eb79ed5639ac9d8d2da34ac12`；JNI `db53023bd573a15f191ec95497ef3e28ad4a55a641fdb09b9997a6bbb92b735e`。APK 内 Host 与上述测试文件一致，安装后拉回完整 base.apk 比较 SHA 一致。

## 运行进展和保留边界

- V5 PID 21495：Commerce/Score/TUS 后进入实体键盘入口，旧 OnlineSubsystem NULL 已越过。
- V6 PID 25460：进入 HMD 初始化，原始错误由 RSP 捕获，VA8 只是错误栈打印后的二次崩溃。
- V7 PID 689：修复跨映射工作区后进入 ErrorDialogUpdateStatus，尚缺该族。
- V8 PID 5282 / gen1 / run `60b472f9d41169b93f99fa775dfd59a7`：ErrorDialog 后进入 WebAPI CheckTimeout，明确 Unsupported，guest return=133。实际画面仍黑，只有初始化阶段一次 present。

最终 V9 运行和清理结果见下方追加记录。全量 2603 行绑定清单与每族未接入项均保留，不把源码存在当成绑定成功，也不把各轮帧计数当成可玩证据。


### 最终 V9 实机结果

PID **10718** / gen1 / run `509a845ec94f88d2c86b2dd57d826935` 越过 WebAPI pump 后，实际进入 `sceHmdReprojectionStartWithOverlay`（`kcldQ7zLYQQ`），因未接入而 Failed/Faulted，operation=42。terminal detail 的 guest return=0 是该故障记录字段，**不是正常返回或成功运行**。累计 guest flip/host present 均 1、host draw 745、guest submission 9；截图仍是黑屏和触控 HUD，无正常菜单、游戏内容或 SBS 可玩验收。

当前桌面 `hmd_reprojection.cpp` 的 StartWithOverlay/Start2dVr 是 stub，Start 也没有完整记录 ABI；不能把其零返回直接搬到 Android。下一批应一起恢复这三项提交记录的 ABI、实际眼图/overlay 的资源及 UV、completion label、事件、显示缓冲与 Stop/取消闭环，复用已有 Multilayer 的 renderer 提交路径。本轮仅确认这个实际调用边界，未恢复该组记录或声称是黑屏的唯一原因。

完整绑定 [bindings-v9.tsv](tetris-online-dialogs-20260921/bindings-v9.tsv) 共 **2603 行**：1146 guest_export、1079 runtime_bound、361 refused、17 not_relocated；refused 去重 **293**。按库列出每一个符号及 importer 的 [family-audit-v9.json](tetris-online-dialogs-20260921/family-audit-v9.json) 保留所有未完成项。Score/TUS 后续在线操作、WebAPI 请求与回调、Auth、Matching、Profile/Invitation/Browser 以及媒体/音频/社交屏幕等均不能由这次启动通过推断为不阻塞。V9 VideoDec2 Load 仍失败，但该次运行继续到 HMD 提交；不推广为整个游戏不需要它。

### 清理

四次 RSP 会话均 stop/清理断点及 forward；临时 debug port/wait 恢复 0，logcat collector 已停止。V9 故障后 native session 已结束、无服务，最后帧仍残留于旧 UI；只在确认空闲后重开前端，最终 Library PID **12725**、session:none、TracerPid=0、无 forward/游戏服务。这个清理不计为正常游戏 UIStop 验收，旧 UI 终态残留问题未修。

global.json 与 host/config.json 均与本轮开始逐字节一致，保留用户 Turnip、Render 0.5、Texture High、SBS、gyro 配置；见 [final-cleanup.json](tetris-online-dialogs-20260921/final-cleanup.json)。本轮没有声称三款可玩、完整桌面兼容、完整回归或性能提升，未提交/推送。
