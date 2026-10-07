# AstroQuest 参考引入：M1 落地与 M2 测量（2026-10-06～07）

对应 [AstroQuest 参考引入规划](../../specs/astroquest-reference-import-20261006.md) 第 4 节的 M1（WP-A、WP-F、C6、D4）与 M2（B1–B6、C1、E1、E2）。设备：AYN Thor（`9c2841a4`，Adreno 740 / Turnip，GPU 封顶 680 MHz）；桌面：Windows，回放用独显。

## 1. 结论

| 项 | 状态 | 数据 |
|---|---|---|
| A1 GS 输入图元补全 | 已合入 | 单测 3/3；血源回放帧哈希不变 |
| A2 GS invocation id（V7） | 已合入，`ShaderBinaryVersion` 38 | 同上 |
| A3 GS 特化比较加入顶点数据大小 | 已合入 | 单测 |
| A4 viewport 槽位保留 | 已合入（光栅器与深度范围模拟一起改） | 回放帧哈希不变 |
| F1–F4、E4 桌面部分、C6、D4（GetPlayAreaWarningInfo 写回） | 已合入 | 编译；F1 压力测试、F3 截断文件单测未写 |
| B1 图像格式列表（UBWC） | 已合入，默认开 | 每帧 GPU 时间约 −8%（见 3.1） |
| B2 跳过重复的 pipeline 绑定 | 已合入，默认开 | VkRecord −7%，回放每稳态帧省约 612 次绑定 |
| B3 深度只读/读写切换打断 pass | 测量后不改 | 血源回放 0 次 |
| B4 HLE 查找去锁 | 已合入 | 未单独测量 |
| B5 BDA 页表按需创建 | 已合入 | 仅在 DMA 关闭时省 512 MiB 显存；Thor 全局开着 DMA，没有变化 |
| B6 每 N 个 draw 提交一次 | 不做 | 血源游戏内 GPU 忙碌 85–99%，没有可填的空闲 |
| C1 XR 重复帧不拷贝 | 未做 | 需要 Swan，本轮未连接 |
| E1 Audio3d 对象空间化 | 已合入，**默认关** | 自检 6/6；坐标朝向未能在设备上确认（见 4.1） |
| E2 7.1 虚拟环绕 | 已合入，默认关 | 血源 8 声道端口运行中开关正常（见 4.2） |

## 2. M1

- **测试**：`tests/gcn/test_geometry_inputs.cpp`（移植自 AQ `e856864`）3/3 通过：V7 来自 `InvocationId`；各输入图元（Point、LineLoop、TriangleFan、Polygon、两种邻接 strip）V0/V1/V3–V6 为顶点偏移、V2 为图元 id；顶点数据大小与 invocation 数参与特化比较。为此把 `shadps4_gcn_test` 的源列表同步到当前 `SHADER_RECOMPILER`（加 `xbyak`、`xxhash`、`Zydis`，`tests/stubs/memory_stub.cpp`）。整套 GCN 测试其余 58 项通过；`pk_add_f16_2/_5` 失败、`bitcmp1_b64_bit32` 中止，与本轮无关，已另起任务。
- **回放**：Thor 上血源 `bb_thor30` 回放，M1 前后帧摘要都是 `9ab8a501bd53`（各 2 次以上）。血源未走到 A1/A2/A4 的新路径；规划中“六款游戏统计 GS 与空槽位”未做。
- **F 组**：内核服务线程在取请求时清零计数；首玩家登录改为看 0 号槽位是否已被占用（原来只看 `controller_count == 0`），取不到用户时报错而不解引用空指针，SDL 打开手柄失败时记录；`sceUserServiceGetEvent` 在 Initialize 前返回 NOT_INITIALIZED，事件队列加锁；存档内存返回文件与请求大小中较大者；`CarveVMA` 断言带区域信息；地址空间预留记录被宿主占用的区间（最多 32 条）；AudioIn `GetSilentState` 不再取端口锁。
- **C6**：OpenXR 建会话、建 swapchain、列举 swapchain 图像改走 `QueueCall`（持队列锁）。
- **D4**：Android `sceVrTrackerGetPlayAreaWarningInfo` 成功时把结果写回 guest；D4 其余两项（时间域、Move 加速度）未做。

## 3. M2：GPU 与 CPU

### 3.1 B1 图像格式列表

**原因**（Turnip 源码 `references/mesa-turnip`，`tu_image.cc`）：本仓所有颜色图像带 `MUTABLE_FORMAT`。A740 是 a7xx_gen1，`ubwc_all_formats_compatible` 为假，没有格式列表时 `tu6_mutable_format_list_ubwc_compatible` 直接返回假，UBWC 关闭。列表只有一种格式，或各格式属于同一 UBWC 兼容类（RGBA8 UNORM/sRGB 同属一类）时保留 UBWC。A740 `supports_uav_ubwc` 为真，storage 用途不影响。`TU_DEBUG=perf` 在血源一局里报告 588 张图像因此关闭 UBWC（RGBA8 206、RGBA16F 179、R11G11B10F 161、RG32F 14 等）。

**实现**：
- `UniqueImage::Create` 给带 `MUTABLE_FORMAT`、非压缩、非深度的图像附 `VkImageFormatListCreateInfo`：自身格式，加上只差传输函数的兄弟格式（RGBA8/BGRA8/ABGR8 的 UNORM↔sRGB）。
- 要建的视图格式不在列表里时，`Image::EnsureViewFormat` 把图像换到不带列表的新 backing 并复制内容（同 create info，`vkCmdCopyImage`），之后该图像的所有 backing 都不带列表。只在新建视图时检查，已缓存的视图不付代价。
- 绕过 `FindView` 的视图：融合回读的打包格式不在列表时改走原 blit 链；MSAA 深度重解释前先 `EnsureViewFormat`。
- 开关：`SHADPS4_IMAGE_FORMAT_LIST` / `debug.shadps4.image_format_list`（默认开），DebugBus `upload_diag format_list on|off`（作用于之后创建的图像）；`upload_diag status` 的 `format_list:` 行给出加了列表的图像数与迁移次数。

**游戏内 A/B**（血源，中央亚楠起点，每轮重开会话，开关交替 6 轮，每轮两个 10 s 窗口；整机随温度逐轮变慢，按每帧 GPU 时间 = 忙碌率 / FPS 比较）：

| 轮 | 模式 | FPS | GPU 忙碌 | 每帧 GPU ms |
|---|---|---|---|---|
| 1 | 开 | 29.84 / 29.67 | 96% / 95% | 32.2 / 32.0 |
| 2 | 关 | 27.88 / 27.12 | 99% / 97% | 35.5 / 35.8 |
| 3 | 开 | 27.49 / 26.96 | 90% / 87% | 32.7 / 32.3 |
| 4 | 关 | 25.52 / 25.05 | 91% / 90% | 35.7 / 35.9 |
| 5 | 开 | 26.90 / 26.43 | 88% / 85% | 32.7 / 32.2 |
| 6 | 关 | 24.63 / 25.89 | 88% / 85% | 35.7 / 32.8 |

开约 32.4 ms，关约 35.1 ms（第 6 轮第二窗口的 draw 数由 1828 降到 1633，场景有变化），约 −8%。每对相邻轮次 FPS 都是开的一方高；第 3 轮起 GpuComm 达 84–88%，CPU 也开始受限，所以 FPS 差小于 GPU 时间差。截图两种模式画面一致。会话中加列表的图像 448–590 张，迁移 1 次。

**回放结果的变化及其原因**：开列表后 `bb_thor30` 回放帧摘要由 `9ab8a501bd53` 变为 `1b28956506fc`，关列表又回到前者，同一构建可重复。差异约占 0.5% 像素、最大 15/255，集中在拱门轮廓等几何边缘，三个抽样帧位置相同。排查：
- 只给 RGBA8 类或只给其余格式加列表，摘要都与关列表时不同，不是某一类格式的问题。
- 强制 `TU_DEBUG=sysmem` 或 `gmem`，开、关两种模式的摘要与不强制时相同，不是渲染模式选择。
- 打开逐事件图像哈希（每个事件后读回、串行化 GPU）后，开、关两种模式完全一致（`08aff3d7ff79`），且与不开哈希时的两个值都不同。

所以差异来自既有的同步/时序问题：某处的结果取决于 GPU 执行速度，UBWC 只是改变了时序；格式列表本身不改变渲染结果。这与外置显卡上“回放偶发单个 MRT 一帧不同、未归因”的现象同类，缺失的屏障尚未定位。另外桌面独显上血源、MHR 回放开关列表帧哈希相同（MHR 有一张 32×80×32 R32Uint 的逐事件哈希在关列表时两次回放之间本来就不同，是原子写的非确定）；MHR 回放中迁移 2 次（A2B10G10R10 以 A2R10G10B10 视图读取）。

**测量方法的教训**：`replay_ab.sh` 只删 `replay_summary.txt`，不删 `frames.txt`；`setprop` 空值在 adb shell 里不生效。最初一次“开关帧哈希相同”的结论读到的是陈旧文件，已作废，改用 `replay_frames.sh`（先删 `frames.txt`）。

### 3.2 B2 重复绑定

`Pipeline::Bind` 记住每个命令缓冲在图形与计算绑定点上最后绑的 pipeline，相同则不再录制（Turnip `tu_CmdBindPipeline` 不比较，每次都重发程序并把描述符和常量置脏）。自己绑 pipeline 的路径（tiling、fault 处理、借出原始命令缓冲）调用 `ForgetBoundPipelines`，新命令缓冲时也清空。DebugBus `pipeline_cache bind_skip on|off`，status 给出跳过次数。回放每稳态帧约跳过 612 次；游戏内同会话交替，VkRecord 线程 12.70 → 11.82 ms/帧（−7%），每对 FPS +0.3–0.4。开关两种模式的回放帧摘要相同。

### 3.3 B3、B4、B5、B6

- **B3**：`BeginRendering` 统计“颜色附件与尺寸都不变、只有深度附件布局（只读/读写）和视图变了”的 pass 切换，`gpu_memory status` 的 `render_pass ... depth_layout_only=`。血源回放 30 帧为 0，不改。
- **B4**：`HleCallRegistry::Find` 改为无锁数组下标访问（上限 65536 个操作，超出在注册时报错），返回裸指针，adapter 生命周期由 registry 保证。未单独测量。
- **B5**：BDA 页表只在设置打开直接内存访问时随缓存创建；否则在第一次需要（DMA 读、取页表根描述符）时创建，并为已驻留的块补写条目。Thor 的全局设置开着 DMA，日志显示页表仍在启动时创建（512 MiB）、回放结果不变；默认关闭 DMA 的配置省这 512 MiB。
- **B6**：前提是 GPU 在翻译期间空闲。血源游戏内 GPU 忙碌 85–99%，不做。

### 3.4 C1

需要 Swan，本轮未连接，未做。

## 4. M2：音频

### 4.1 E1 Audio3d 对象空间化

- 移植 AQ 的 `Spatializer`（Brown–Duda 头模型：分数延迟的耳间时差、头影搁架滤波、后方低通、块内参数渐变）。接在桌面与 Android 共用的 `ProcessMixQueue`：带 POSITION 属性、非 passthrough 的单声道对象走头模型，其余照旧；混音缓冲改为端口内复用。对象 RESET 时重置滤波状态。
- 自检（独立程序）6/6：右侧声源右耳早 31 个采样且能量高；左侧对称；正前方两耳同时、能量相等；后方比前方暗。
- **坐标朝向未确认**：AQ 认为 +Z 在听者后方，本仓 OpenAL 路径按 +Z 在前方（取反）。为此加了统计：按能量记录带位置对象在标题坐标 ±X、±Z 上的分布（DebugBus `audio3d_spatial status|reset`）。在 Thor 上：TMNT 进巢穴，Audio3d 混音块为 0，它不走 Audio3d 对象混音；MHW 从标题、菜单到开场剧情约 3.2 万个混音块，带位置与不带位置的对象块都是 0，只有 bed。规划里“MHW、MHR、TMNT 用 Audio3d 对象”的说法不准确。
- 因此默认关（`debug.shadps4.audio3d_spatial=1` / `SHADPS4_AUDIO3D_SPATIAL=1` 或 DebugBus `audio3d_spatial on` 打开，`front_z +|-` 选朝向，默认 `+` 即沿用 OpenAL 路径的约定）。统计不论开关都收集，遇到有带位置对象的游戏（PSVR 游戏、MHR 实战）时读出能量分布即可定朝向。

### 4.2 E2 7.1 虚拟环绕

- 移植 AQ 的 `SurroundVirtualizer`：7 个虚拟扬声器（±30°、0°、±110°、±150°）各过一个头模型，LFE 低通后混入，整体限幅。读取与 `PrepareAudioStereo` 相同（端口声道布局、逐声道增益、未对齐的 guest PCM）。guest 逻辑顺序 L R C LFE Ls Rs Le Re 中 Ls/Rs 放在 ±110°、Le/Re 放在 ±150°。
- 每个 8 声道端口一个实例，规划缓冲在构造时分配，处理时不分配。Oboe 端 `Prepare` 由 GuestAudio 在端口锁下按块顺序调用；cubeb 端只在立体声设备上使用。
- 默认关：`audio_virtual_surround on|off|status`，`SHADPS4_VIRTUAL_SURROUND` / `debug.shadps4.virtual_surround`。Swan XR 会话默认打开留待 Swan 验证。
- Thor 血源（MAIN、BGM 两个 8 声道端口）运行中打开 20 s 再关，游戏继续出帧、无错误日志。听感未经人确认。

## 5. 顺带发现：Cemu 反复弹“屡次停止运行”

Thor 上 `info.cemu.cemu` 当天 08:55 起崩溃 13 次以上，崩溃框盖在游戏画面上。原因（dropbox `data_app_crash`）：`CemuApplication.onCreate`（CemuApplication.kt:56）无条件调用 `DebugDumpService.start()`。进程被系统在后台拉起时（`Start proc ... for ` 无组件，约 12 分钟一次；另有一条 3 天前的已启动服务记录 `DebugDumpService`，进程死后会被按粘性服务重启），Android 12+ 抛 `BackgroundServiceStartNotAllowedException`，应用在 onCreate 就崩。

- `am force-stop info.cemu.cemu` 清掉了残留的服务记录，但后台预启动仍会触发崩溃（之后 09:30、09:42 各一次）。
- 该服务来自 Foundation 模板 `foundation/modules/debugbus/templates/android/DebugDumpService.kt`。已改模板：`onStartCommand` 返回 `START_NOT_STICKY`；`start()` 捕获后台启动被拒（`IllegalStateException`）并返回 false，由宿主在 Activity 回到前台时再调用。Cemu 源码不在本机，需要在 Cemu 中同步这份模板并改调用位置。Foundation 的改动尚未提交。
