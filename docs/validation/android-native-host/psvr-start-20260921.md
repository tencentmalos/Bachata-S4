# AYN / HMD Start 提交族与每眼采样（2026-09-21）

用户提出 `sceHmdReprojectionStartWithOverlay` 及相关入口可能导致三款 PSVR 游戏异常。本轮确认 **Tetris 的实际阻塞确实在这个入口**，已补齐 Start / Start2dVr / StartWithOverlay 的 Android SBS 提交路径，并修复与 Multilayer 共用路径丢弃每眼 UV、sampler 的问题。三款均已用最终 APK 在 AYN Thor `9c2841a4` 运行；**仍未达到可玩或完整 SBS 验收**。

源码为 `feature/malos/beat_saber_fix`，HEAD `63b6d560dd43af031ec7a5de64ae6204beea5de2` 加全部已有未提交修改。本轮未 commit/push。证据与关键源文件 SHA 见 [manifest](psvr-start-20260921/manifest.json)。

## 调用证据与假设边界

完整绑定图显示：Beat Saber 只导入 StartMultilayer；Sports 导入 Multilayer 与 Start2dVr；Tetris 导入 Start、Start2dVr、StartWithOverlay。不能把三款都解释成调用同一个 WithOverlay 入口失败。

在旧 APK 的 Tetris PID12725 / gen1 / run `1d6360288f52522a9ce363c9483cc743`，Guest RSP 于 main+`0x59b36a`（基址 `0x400000`，运行地址 `0x99b36a`）捕获真实 WithOverlay 调用。现场代码字节与归一化主 ELF 一致；参数为：128 字节 base 记录、56 字节 pose、sequence=2、96 字节 overlay 记录、reserved=0。release label 地址 `0x3000030208`，原值1。源码和现场资料未把 guest 地址当 host 指针。

固件11.00 `libSceHmd.sprx` SHA `c5005afcb8bddeaa6de23cd581eabe080087bb45b36c8a9dc89cf6af227f0e71`；归一化 ELF SHA `0c05f07820a93be191a1d50ccb74d16621d1a8c012128c77c45f9e8120b4c6de`。Start / WithOverlay / 2d 的入口分别为 `+0x161e0 / +0x16c90 / +0x17690`，参数个数为4/5/3。记录中的第三个地址是 **16字节 GNM sampler**，随后是两组 float4 的 tangent-to-UV 变换，不是旧实现推测的 scale 指针。

Reverse Study 打开分析文件后索引函数为0，decompile 返回 NOT_IN_FUNCTION；本轮转用有界固件指令与真实 guest 参数交叉核对，未宣称成功反编译。RSP 缺少完整 PID/start-ticks/Build-ID 握手：使用外部运行状态、模块基址和现场代码核对，不能称完整进程身份认证。会话、断点、forward 和临时 study workspace 已清理。

## 实现范围

- 三个 legacy Start 入口采用正确的记录布局与保留参数位置，检查嵌套描述符、sampler、label 对齐、保留字段、selector/flag 组合及有限 UV。新增准入和真实 renderer 提交，不采用桌面 stub 的零返回。桌面声明同步 ABI；没有对应 transport 时返回明确不可用，未完成桌面渲染适配。
- legacy 与 Multilayer 共用拥有数据的 `VrFrameSource`，保存最多四张眼图描述符、各自 sampler 与 UV。base 和 overlay 分别计算采样坐标，保留独立切片和动态分辨率裁剪。按已向 guest 返回的同一组 FOV 将 tangent-to-UV 转为归一化 scale/bias；同一个 image/view 不再决定它是否应被整幅采样。
- Tetris 的 base 描述符是2688×1512，实际 UV 为每眼宽 `3/7`、高 `6/7`，右眼起点 `3/7`：有效区域为2304×1296，每眼1152×1296。直接整图采样会读入无效区域。overlay 使用自己的描述符与 UV，不能继承 base 的 atlas 判断。
- 复用 TextureCache 的实际 sampler，保留 mirror / clamp-border、过滤等行为；自定义 border table、比较/非归一化采样及当前不支持的字段明确拒绝。生产后处理保持单次 draw，输出 Alpha 仍为不透明，overlay 仍采用既有 premultiplied 合成。
- Unity 的实际 Y 变换为 `scale=-1,bias=1`，Tetris 为正 scale。删除旧 presenter 统一 Y 翻转，避免在已包含方向的变换上翻转第二次。V3 的倒置画面作为反例保留。
- 输入纹理和 completion label 通过逐映射段的 generation 快照统一 AcquireDataBatch；映射洞、权限错误、溢出和同地址重映射仍拒绝。只有 GPU 读取和 host submit 退休后才清零 label，未增加 GPU idle 等待。
- 显示缓冲按实际 busy slot 管理，支持乱序退休；完成回调只执行一次，重复回调不能释放后来复用的槽位。回调所需分配先于槽位占用，提交异常回收占用。Stop/Finalize 继续等待本会话在途读取，Stop 后重新提交会恢复活动状态。

这是一组 **SBS 原图传输** 合同，不含镜片畸变、头部时间重投影或原生头显时序。2d 提供双眼同一归一化矩形的 SBS 表示，只有定向测试，本轮未确认真实游戏走该分支。额外 depth/第三层、Multilayer2、WideNear、capture 等没有因此完成；已有 SetOutputMinColor 桌面 stub、Tracker 虚拟阶段等边界也未被此次绑定数量掩盖。pose 被安全复制，但 SBS 不执行 pose warp。

## 定向验证与安装

| 最终 V4 测试 | 检查 / 失败 |
|---|---:|
| Reprojection CPU：legacy ABI、非法记录、裁剪/负Y、sampler、生命周期、乱序/重复退休、映射身份 | 76 / 0 |
| Qualcomm：实际生产后处理 shader、array slice、独立 overlay、裁剪、透明 border、正/负Y 非均匀行 | 17,408 / 0 |
| Turnip：相同像素读回验证 | 17,408 / 0 |

V3 的74/0、两驱动各15,360/0也保留；V1/V2是构建失败，修复了 BitField 枚举取值和测试初始化写法，不计为通过。V4 native/APK 构建及 `git diff --check` 通过。未做全游戏回归或性能 A/B。

最终 APK SHA `d3599892d0cef3977a018754fe73360cb9fd9e3d6647967d09ea7c8c3cd1bc80`，Host `f1ed3e7b5eb2170cc81f43d2baa3e5a6213c207c51069859daef36dbfff31bc6`，JNI `4c1c116b023091f6a49244bd71b438f50ddda1c72d5de292822db26273e24ff0`。APK 内 Host 与测试文件一致；安装后读取完整 base.apk 核对 SHA 一致。保留用户 Turnip / Render0.5 / TextureHigh / SBS / gyro。

## 三款实际效果

| 游戏 / 最终 V4 会话 | 观察 | 结束 |
|---|---|---|
| Beat Saber，PID15411 / gen1 / `29f4a5e64c639695a3b4c6fb29711f62` | 安全提示、背景、光剑正向可见；文字/光剑仍重影。触屏 Cross、R2 后未越过 Continue。不是正确 SBS 或可玩 | 2008 presents，UIStop → Stopped/user_stop；guest return2147614724，不是0 |
| AllInOneSports，PID16038 / gen1 / `0436efdbf81aa4b4101644de7734449b` | 安全提示方向恢复，但左侧重复图像、右侧黑，控制器图案仍异常；Cross/R2 未推进。源为960×1080 array，UV每眼约(1,1,0,0)，需继续查实际切片写入/实例与 guest shader | 3310 presents，UIStop → Stopped/user_stop；guest return18446744071562199125，不是0 |
| Tetris，PID18526 / gen1 / `95eeae59ee2235c72902036931d7cd60` | 越过旧 WithOverlay 拒绝，持续提交；确认提示之后仍黑屏。Cross 未进入菜单，不能用帧数证明场景正确 | 1421 presents，UIStop → Stopped/user_stop；guest return68724916224，不是0 |

Tetris **V3** PID8528 / run `8f918b1c1a03d31572833d1fe588cc05` 首次显示双眼提示框和 X/OK，文字倒置；按 X 后提示消失并持续黑屏，2171 presents 后 UIStop/return0。该可见提示只属于 V3，不能写成最终 V4 的截图验收。最终 V4 的首次运行 PID17277 / run `57e734f37dd3b456483b54801516e018` 在启动阶段 Guest-50 出现 SIGSEGV / PC0，native 回溯第二帧是 FEXMemJIT，未获精确 guest 原因；同版重启为上表 PID18526 后没有在该窗口复现，但也没有解决黑屏。保留崩溃反例，不能声明稳定性通过或把 PC0 直接归因 HMD。

截图：[Beat](psvr-start-20260921/beat-v4.png)、[Sports](psvr-start-20260921/sports-v4.png)、[Tetris V3提示](psvr-start-20260921/tetris-v3-visible2.png)、[Tetris V4黑屏](psvr-start-20260921/tetris-v4b-after-input.png)。UI 状态与日志均按版本/PID区分；host 文件是累计日志，摘录本身不具备独立 PID 身份，须和相应运行状态结合。

此前 [Beat 原始眼图分析](psvr-progress-20260920.md) 已观察到 host 后处理之前的重影；本轮最终图像仍异常，进一步说明 HMD 提交接口修复不足以解决 Unity 输出。下一步应分别追踪 Beat 的最后两次 guest 合成的顶点/UV，以及 Sports array 切片写入；Tetris 则需确定黑屏时 base/overlay 内容及启动 PC0 的实际 guest 调用链。Videodec2 provider 加载仍失败，但没有证据将其认定为黑屏唯一原因。

## 完整导入清单与清理

| 游戏 | 全量行 | runtime_bound | refused行 / 唯一符号 |
|---|---:|---:|---:|
| Beat | 1939 | 889 | 256 / 243 |
| Sports | 2303 | 1045 | 278 / 253 |
| Tetris | 2603 | 1082 | 358 / 290 |

同名证据目录保留三款完整 `*-bindings-v4.tsv`、逐库每个拒绝项及 importer 的 `*-family-audit-v4.json`；Tetris以最后一次 V4b 图为准。绑定不代表完整语义或实际执行覆盖。NP/WebAPI在线、媒体、社交屏幕及其他拒绝族未因本轮启动推进而被判定为不阻塞。

正常停止本轮会话后，确认 session:none 才重开前端清除既有终态残留。最终 Library PID19927 / TracerPid0，无游戏服务、无调试 forward，guest debug port/wait=0；本轮 logcat collector 关闭，无自有 scrcpy 或 GPU capture 会话。global.json 与 host/config.json 和开始时逐字节相同。清理详情见 [final-cleanup](psvr-start-20260921/final-cleanup.json)。停止后的旧画面/UI残留仍未修复。
