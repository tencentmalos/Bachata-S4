# TMNT Turnip / Qualcomm RDC 对比与深度状态兼容修复

后续补充：[新 RDC / Turnip 源码 / 雾深度范围定位](fog-depth-range-turnip-2026-09-17.md)。
修复后 Qualcomm 完整 RDC 已成功，510点中深度1.0由498降至0；下文最初补抓失败记录保留。
残余雾已定位到 Turnip event4397；viewport 半区间深度映射与 soft-particle 深度还原不匹配
得到 API/诊断 shader trace 支持。临时撤销该映射明显减弱硬边，但尚未部署正式 renderer 修复。

## 结论

**复验修正：仅部分修复。** 高通严重的栏杆/窗户块状破面已明显消除，但雾仍有大片
直线、三角形及条带状硬边。用户指出后，重新放大 Turnip 已有 RDC 的最终输出，
也能观察到类似残留。因此 Turnip 是深度缺失问题的对照，不是完整画面正确性基准；
此前“雾边恢复正常”“画面验证通过”的整体表述过度，以下证据只支持局部改善。

已取得并成功回放 Turnip 屋顶对照 RDC；与已有 Qualcomm 坏帧对比，严重破面问题收窄到
**图形 pipeline 绑定后的动态深度状态提交路径**。在 Qualcomm proprietary 驱动上，
每次绑定 guest graphics pipeline 后重新下发 depth-test enable、depth-write enable、
depth compare op，屋顶栏杆、窗户附近的严重块状破面明显消除；雾效硬边尚未修复。

这是一处经实际画面验证的兼容修复，不是 Qualcomm 驱动内部根因的完整证明。
没有修改 guest shader、Int64 lowering、雾效、纹理格式、队列同步或 FEX；
没有新增 CPU/GPU 等待、render pass 或 barrier。直接和间接 draw 路径都覆盖；
普通 APK 实测覆盖当前 TMNT 屋顶，不声称所有游戏/驱动版本/间接 draw 均已实机验收。

## 固定环境与资料

- AYN Thor，`9c2841a4`，Adreno 740，Android 13/API33，4 KiB。
- Host/JNI RelWithDebInfo，FEX Release，APK playstoreDebug。
- Turnip：Mesa 26.0.0-devel (`5ac41be677`)，driver SHA
  `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。
- Qualcomm：512.676.53，Vulkan 1.3.128，build `69e13475cb`。
- 配对 RenderDoc MCP/native/Android replay：`6ae929af16150fe39fd13f2c41b4ee00b60c704b`
  加已有 replay-loader patch。所有 Vulkan replay 都在设备上执行，没有 macOS Vulkan fallback。
- 本轮材料：`build/turnip-compare-20260917/`；`comparison.html` 是双图、共用色标的深度采样对比。
- 保留已有大批工作区修改；本轮没有 commit/push，也没有完整回归。

## 正常 Turnip RDC

`build/turnip-compare-20260917/tmnt-rooftop-turnip.rdc`

- 1,208,629,838 bytes；SHA-256
  `895f7f984f718c5336204de17a3b38ed8ff5e4eb7a8b269c5b4a716ad56a3143`。
- PID27866 / generation1 / run UUID `c96b0bea8d83467a2ff599688938db15`。
- coordinator Ready，cleanup_pending=false，guest flip1411→1412。
- 抓取前普通截图确认实际屋顶，停止自动按键后 arm；抓取后正常 UI Stop 到 user_stop。
- APK `7c2f47ce…`，与之前恢复的 marker 包相同；这份正确帧尚未包含本轮兼容修复。
- 同一 Turnip 驱动回放成功，MCP `review_texture` image block 与实际截图独立确认画面。

对照旧坏帧：`build/graphics-tooling-20260916/tmnt-rooftop-writing-timeout.rdc`，
1,167,676,491 bytes，SHA-256
`4e1e908e4a1ecb1cdba9c16ed7ad34bb5701bc200904795911b2e8a32b8bd455`。
文件名 timeout 不代表它无效；已在对应 Qualcomm ICD 上成功回放。
旧帧 APK2126d946、新 Turnip 帧 APK7c2f47ce，且动画时刻不同；不是精确同帧 A/B，
不能对 PNG 做逐像素等价或由两张帧图推导性能增益。

## 从输出反查深度

用 guest VA、格式、shader hash、bindings/usage 对齐，不能跨 capture 复用数字 ID：

| 语义 | Qualcomm 旧坏帧 | Turnip 正常帧 |
| --- | --- | --- |
| Guest 输出 / Host 采样 draw | resource387 / event4716 | resource411 / event4704 |
| Tone-map 场景 RGBA8 | resource6656，首次 draw3834 | resource7014，首次 draw3796 |
| HDR 场景 | resource8806 | resource9113 |
| D32S8 深度，VA0x27e080000 | resource8784 | resource9089 |
| R32F 深度副本，VA0x27f3d0000 | resource10232 / copy3356 | resource10507 / copy3291 |

雾效之前，R32F 全屏每64像素采样（510点，mip0/slice0/sample0）：

| 数值 | Qualcomm | Turnip |
| --- | --- | --- |
| 清屏值1.0点数 | **498/510** | **0/510** |
| 最小值 | 0.9790017605 | 0.9590413570 |
| 最大值 | 1.0 | 0.9872899055 |
| 平均值 | 0.9995602108 | 0.9795854785 |

原始 D32S8 的 pixel_pick 同样显示差异，排除了“只在雾效 depth-copy shader 才出错”
这一狭窄解释。之前等价替换 copy shader 无效的证据仍成立。
例如复制阶段 (500,500) 为 Qualcomm1.0、Turnip0.9802494。
正常帧明确覆盖这些位置，因此不再只依赖旧帧中1.0值去猜测应该有几何。

## 必须保留的回放限制

1. 试过让 Turnip 交叉回放旧 Qualcomm RDC，因旧帧要求
   `storageBuffer8BitAccess` 而被拒绝（ResultCode16）。没有绕过 feature 检查；
   后续全部使用 capture 对应驱动。失败日志保留在 `old-on-turnip-logcat.txt`。
2. 默认 WithoutDraw + OnlyDraw 拆分回放，在 Qualcomm event1036、(500,500)
   可读到0.98645514，而 event1045又为1。**不能把这个差异当成真实的“1036写好、1045清掉”。**
   临时使配对 macOS controller 使用 `eReplay_Full` 后，1036本身也为1；说明
   OnlyDraw 的状态重新绑定影响了读数。临时工具源码及 dylib 已逐字节恢复。
3. 完整回放的最终 depth-grid 与普通回放一致；最终 guest PNG 也完全一致：
   `a4cf5d0ab0c3fd87f8919107fe9287992460263cf7e7ddb0a950f77152dd001c`。
   因此最终坏图和大面积缺失深度不只来自这个单 draw 读取假象。
4. `guest_frame_structure` 保留 shadPS4 marker 树，但当前解码器给出 unknown domain；
   没把其 guestDrawCount=0 解释成没有 guest draw。完整 marker 文本/绑定证据仍可用。
5. 旧 pixel_history 会使 replay 崩溃，本轮没有重试。viewport0 的粗摘要仍是未知值，
   不是实际0宽 viewport。

## 最小化验证与最终代码

`src/video_core/renderer_vulkan/vk_rasterizer.cpp` 的 Draw、DrawIndirect：

1. 诊断版：Qualcomm 每次 bindPipeline 后 invalidate+commit 全部动态状态。
   普通 APK 实际屋顶恢复，`rebind-current.png`；APK `eb24cf87…`，
   host `67f82078…`。之后实际 UI Stop 到 Stopped/user_stop。
2. 缩减版：仅重发 depthTestEnable / depthWriteEnable / depthCompareOp；
   其他 driver、动态状态缓存与 shader 全部维持原路径。
   实际屋顶同样恢复，`depth-current.png`。APK `5884b68f…`，host `3c5d85d1…`，
   JNI仍为 `d471c459…`。两个版本都没有重新生成 guest shader 作为修复条件。
3. 最终保留第二种路径；无需修改 shader cache key。全状态重发试验已移除。
   两个 graphics draw 入口都有相同保护；Turnip 不进入该分支。

[Vulkan 的动态状态生命周期](https://docs.vulkan.org/guide/latest/dynamic_state.html)
允许动态状态在命令缓冲中保持，直到被修改或被相应静态 pipeline 状态失效。
当前结果支持在这版 Qualcomm 的 pipeline 切换/状态缓存组合上增加兼容处理，
并不支持“Vulkan 本来要求每 draw 全量重发状态”或“必须增加全局同步”的结论。
实际底层寄存器/驱动 compiler 原因仍未取证。

## 修复后补抓的失败边界

缩减版的补抓 request1 / PID20744 / UUID `a947ceeda43e4cdb8822b75c1a6624e3`
在 RenderDoc 准备655个 initial-content资源、512 MiB readback窗口及持久映射拷贝时，
被系统 lowmemorykiller 终止。Android exit-info 明确 `reason=3 (LOW_MEMORY)`；
日志说明 critical memory pressure，signal9，没有据此声称 native crash。
留下约589MiB残缺 RDC，没有 Ready/完成 sidecar，不用于任何修复后 replay 验收。

所以：**普通 APK 局部破面改善已验证，雾效硬边仍未修复；修复后高通深度数值的完整 RDC 复验尚未完成。**
本轮用户要求的正常 Turnip RDC 已完整取得，不受该补抓失败影响。
自动 warmup 的 STOPPED_UNVERIFIED 只是操作脚本状态；画面判断来自实际截图和
明确场景，不冒充 ten-minute/gameplay/performance 验收。

最终部署、恢复状态与完整SHA见下节及 `final-artifacts.json`。

## 最终部署与清理

- 最终 APK SHA-256：`5884b68ff47e623c7af99120d9cfcb663f62c2564cf7f4b6e6d28d9f6157cfcd`，
  设备安装文件SHA已与本地匹配。最终注释整理后的重建未改变二进制。
- Host SHA-256：`3c5d85d10fd030b9fd9d348965e05bb86c05fdb391aeec1c69042338b88ffba2`。
- JNI SHA-256：`d471c459b876b04b123f3fd6281d2292e3425e9a7084f15fd07f96d5fdbcc2c2`。
- 最终 renderer 源码SHA：`784136833756e7378f09ff1c75b2d5340d011c00492dabcd7f40dbb63bcf42a0`。
- 已恢复全部5个GPU layer settings为空、replay loader property为空。
- 最终重新启动在 Turnip，PID25246/gen1/UUID `9c381abcc975fe7ca24dfce50e67708b`，
  实际屋顶截图 `final-turnip.png` 正常；RenderDoc API未加载、TracerPid0，自动按键已停止。
  游戏留在屋顶供用户验证，不是再次GPU抓帧。
- 临时 RenderDoc源码/dylib与前值完全一致，见 `tool-restoration.json`；没有发布MCP新版本。
- 最终增量见 `final-source.patch`，只有两个 graphics draw 入口的 Qualcomm 深度状态重发；
  其余之前的未提交修改均保留。host/APK构建及定向画面检查通过，`git diff --check`通过。


## 高通普通 APK 复验与 cache 归因修正

本节覆盖上一节“最终 Turnip”的设备状态。按用户要求，先 UI Stop 确认 user_stop，
再完整重启进程、切换 system 驱动，复用已安装 APK5884b68f，没有清除或重建 shader cache。

- PID28479 / generation1 / UUID `b3e2e4f5c7742737240987b82b7319bc`。
- 实际 driver source=system，Adreno740，build69e13475cb，shaderInt64=0。
- RenderDoc API未加载、5项capture settings保持空、TracerPid0；没有新抓RDC。
- warmup126.8s已到Leonardo屋顶对话，144.9s出现ATTACK；之后短暂stick-left输入，
  人物和相机位置发生变化。只做本场景确认，脚本最终STOPPED_UNVERIFIED，未冒充完整验收。
- 截图与会话记录：`build/turnip-compare-20260917/qualcomm-confirm/`。
  最新final-session中guest_flip3795、host_present3793，实际画面约12–13FPS；不声称性能改善。
- 当前游戏保持高通驱动在屋顶运行，自动输入已停止。

源码核查：guest graphics pipelines始终将三个depth状态声明为dynamic；
`DynamicState::Commit`按dirty位跳过未变值。两个static-depth blit路径结束都会Invalidate，
新command buffer也Invalidate；postprocess之后立即Flush，ImGui走独立present scheduler。
已检查路径未发现明显host dirty-cache失效漏项，但这不等于穷尽所有路径。

因此只能说：**异常与graphics绑定附近depth状态的应用/保持有关**。尚未证明驱动内部cache
具体缺陷，也未证明只有pipeline handle改变时才发生；当前workaround是每次bind后重发。
没有证据支持“磁盘shader/pipeline cache损坏”。详见`state-cache-audit.json`。

残余雾硬边在当前高通实际截图，以及Turnip RDC输出resource411/event4704均可见。
再次通过MCP review_image检查原PNG的(700,280,700,470)裁剪，原图SHA仍为2edfd7b6。
已有Turnip resource_usage记录显示：深度副本10507在event3291生成，随后一批draw
（3309–3724，非区间内每个event）读取它并写HDR场景9113，event3796采样9113做后处理。
这只是残余问题的候选检查范围，尚未逐draw定位首个错误writer或证明具体雾shader。
不能凭三角形外观直接认定插值、混合、深度精度或cache是根因；尚未新增生产修复。
