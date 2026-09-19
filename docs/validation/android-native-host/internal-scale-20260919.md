# Android Internal Scale 实现与验证

日期：2026-09-19。适用本轮提交快照；不包含完整游戏回归。

## 行为与配置

按照用户最新要求，guest shading rate 恢复 high / 1×1，FDM 保持关闭。新配置 `gpu.internal_scale` 提供 0.5、0.75、1.0；Android 默认 0.5，修改后重启游戏生效；桌面原生配置的默认值仍为 1.0。全局和游戏配置沿用现有覆盖规则，Android 启动服务在创建 renderer 前通过 JNI 设置倍率。导出原生配置为 `GPU.internal_scale_percent = 50 / 75 / 100`，不是浮点字符串或枚举下标。DebugBus `internal_scale status` 只查询，不在运行中替换倍率。

倍率同时作用于符合条件的 guest 渲染目标和加载贴图。`ImageInfo` 的 guest 地址、大小、pitch、tile、mip offset 与脏页范围保持原定义；只有 Vulkan backing 的 extent、mip 数和必要时的压缩格式改变。1920×1080 的可缩放目标分别分配为 960×540、1440×810、1920×1080。

| 资源 | 0.5 | 0.75 | 1.0 |
| --- | --- | --- | --- |
| 可过滤的未压缩 2D 贴图 / 单采样 RT | 物理缩小，上传脏数据时重采样 | 同左 | 原路径 |
| BC，已有后续 mip | 丢弃 guest mip 0，guest mip 1 成为 host mip 0，仍为 BC | 支持的 LDR 格式重采样为 ASTC 4×4 / 6×6 | 原生 BC 上传 / 硬件采样 |
| LDR BC，只有一级 mip | GPU 重采样并编码为 ASTC 4×4 / 6×6 | 同左 | 原路径 |
| SNORM / HDR BC | 有 mip 时保留原格式丢一级；否则原尺寸 | 原尺寸 | 原路径 |
| storage / atomic / integer / volume / 小于 16 的维度 / cube 特殊处理 | 原尺寸或首次使用时提升回原尺寸 | 同左 | 原路径 |

这是按资源适用性执行的缩放，不承诺所有 guest 资源都缩小。当前没有通用的 BC 不支持设备的软件解码 fallback，依赖当前 Ayn 已验证的原生 BC 采样能力。BC→ASTC 保持硬件压缩：原 BC1/BC4 的 8 字节块选 6×6，BC2/3/5/7 的 16 字节块选 4×4，避免 0.75 倍 BC1 转 4×4 后反而膨胀。以大尺寸单级近似计算，0.75 的 BC1→ASTC 6×6 为原字节数的 50%，16 字节 BC→ASTC 4×4 为 56.25%；实际按块向上取整，Vulkan allocation 还受页/布局对齐影响。0.5 有 mip 的路径保留 BC。

## 渲染、shader 和别名

- Render area、viewport XY/宽高和 scissor 同步缩小，保留已有负 viewport 高度和深度映射。FragCoord XY 转回 guest 坐标域；原有深度 / fog 修复保留。
- 普通采样直接读取缩小的 backing。整数 texel fetch 按真实 host/guest mip 尺寸映射；尺寸查询返回 guest 原始 T# 维度和级数。mip-drop 的显式 LOD 和 view base/min LOD 同步转换，尾部 mip 查询钳制到真实存在的级别。
- 128 字节 push constant 中用 8 字节传递每个绑定及渲染倍率，前 31 个统一 descriptor binding 支持纹理倍率；超出后保留原尺寸。shader profile 与二进制 / metadata 版本共同隔离旧缓存。
- 精确 texel offset、LOD 查询、可写 storage、整数 / cube 特殊访问使用原尺寸。带 fragment 副作用、MSAA 或混用原尺寸附件的 pass，将相关附件提升为原尺寸，避免少执行 guest 数据写入或附件尺寸不一致。
- 兼容尺寸的图像复制、深度与 R32 的 buffer 中转复制使用实际 backing 尺寸。mip 别名、尺寸不兼容的 reinterpret/copy/resolve、guest readback 保守提升到原尺寸。修复 buffer 中转多 mip 复用同一 offset 的问题，防止后一级覆盖前一级。
- 提升是单向的；旧 image/view 按 scheduler 的 GPU 完成和 host submit 返回共同退休，没有新增 device/queue idle。未压缩 GPU 内容经 blit 保留，压缩采样资源在写入前从原 guest blocks 重建原尺寸 backing。
- Presenter 读取实际 backing 尺寸，继续使用已有上屏路径；没有添加第二次全屏缩放 draw/blit，也没有重新启用 FDM。

## Foundation ASTC 模块

通用实现位于 `foundation/modules/texture_codec`（MIT）：支持 4×4 / 6×6 block 的 GLSL compute 编码器、请求布局和 Vulkan pipeline/dispatch 封装。宿主继续负责 shader 编译、descriptor、图像/块 buffer 分配、屏障和异步生命周期，Foundation 内部不提交队列、不等待、不涉及 guest 地址和 tile。

编码器在采样源 BC 图像时直接重采样，输出合法 ASTC 4×4 / 6×6 blocks，随后 copyBufferToImage 到实际缩小的 ASTC 图像。源全尺寸图像是本次脏上传的临时资源，随提交退休；没有永久保留 RGBA 中间纹理，也没有每帧无条件重编码。首次处理及脏纹理更新仍有编码和临时内存开销，没有实现跨启动磁盘压缩缓存。

快速编码采用单分区主轴端点拟合：opaque 为 RGB 端点 + 3-bit 权重，alpha 为 RGBA 端点 + 2-bit 权重；均使用 8-bit 端点。4×4 权重网格适配两种块尺寸，保留块内梯度、反相关颜色和透明度，支持正确的线性↔sRGB 转换。它不是 astcenc 高质量离线搜索，复杂细节可能出现额外损失；没有 HDR/SNORM 编码。

对照本地 Azahar 的 `EncodeAstc4x4` 和压缩后释放缩放图像的生命周期；其现有 shader 为每块平均色 void-extent，这里没有复制该 GPL 实现。位布局通过本地 Arm astcenc 物理块/权重解码源码核对，实际是否合法由设备采样回读验证。

Foundation 基于与 origin/main 相同的 `17dd6854` 建立 `codex/shadps4-internal-scale`，保留原有 staged audio/input 和 profiler SDK 改动。本轮交付先推送 profiler SDK 与 Foundation 分支，再由主仓固定其提交；没有合并至 Foundation main。

## 验证记录

- Android 配置定向测试：InternalScale 5、catalog 3、JSON codec 4、配置导出 5，共 17 项通过。
- 第一版真实 GPU probe：Qualcomm 系统驱动与 Turnip 各 99,492 checks / 0 failures，覆盖生产 Image 上传/重采样/退休和生产 SPIR-V fetch/query，65×49 奇数尺寸、三 mip、两数组层以及回原尺寸内容保留。
- 第一版 APK 的 0.5 TMNT 屋顶：`build/validation/internal-scale-20260919/half-initial`，PID 23320 / gen1 / UUID e9feb18d3ae062c905c74a2765cc5a54；实际移动与转视角、MOVE→ATTACK，GAMEPLAY_REVIEWED，UI Stop 正常结束。该版本不包含后来补入的 Foundation ASTC，约 21–22 FPS / guest GPU 45 ms 只是观察值，不是最终版或匹配 A/B 结论。
- 压缩路径第一轮测试暴露 ASTC 格式属性未缓存断言，已补缓存和设备 compression features 启用；失败日志保留，后续扩展验证结果以本节追加记录为准。

### 扩展设备 probe

最终 Qualcomm 系统驱动与 Turnip **各 1,563,042 checks / 0 failures**。使用实际宿主 `Image`、上传/原尺寸提升、多 mip buffer 中转复制、SPIR-V 整数 fetch 与逻辑尺寸查询，以及 Foundation encoder + 硬件 ASTC 解码采样回读。覆盖 100/75/50、奇数 65×49、三 mip、两数组层、BC1/BC3、BC1 单 mip、BC1 sRGB、BC mip-drop、ASTC 4×4 / 6×6、透明度与反相关颜色。probe 的 case4/5 是测试编号（BC1 单 mip / BC1 sRGB），不是 BC4/BC5 格式验收。

3 份生产 image shader SPIR-V 和 Foundation GLSL 编译出的 SPIR-V 均通过 Vulkan 1.3 `spirv-val`。原始日志和 shader 位于 [evidence/internal-scale-20260919](evidence/internal-scale-20260919)。

画质限制保留：一条端点轴无法同时准确表示独立的 R=X / G=Y 色彩平面。首次把这一低 mip 极端图案按每通道 0.08 断言，出现 5,660 处超限；这是该快速编码模型的画质限制，没有当作非法 ASTC 块修复。最终继续保留独立双轴图案，按块内变化幅度设定明确预算并输出实际误差，同时增加严格的反相关单轴、alpha 和 sRGB 检查。最终极端小 mip 的独立双轴峰值误差：4×4 为 0.218，6×6 为 0.363；6×6 带 alpha 的 sRGB 单轴峰值 RGB 误差 0.101、alpha 0.0375。原失败和扩展日志一并保留。**零失败不意味着无损或高质量编码。**

扩展 GPU probe 对应 APK SHA256 `8b0cc12bc6762d76662fec6f9e1aac26dd690a178f63ae2bf2336f8c55cfb814`；打包 host SHA256 `a9f742c9c1bd73503c19ee3c34042c94a7884ea47ba7642de8faf84e68c2e9cc`。打包与 GPU probe 使用的 host ELF Build ID 同为 `48f99d0853f46e4e07f49aea4b69fbe3e59390e0`，host 为 RelWithDebInfo。该 APK 已安装到 Ayn，后续 overlay 版本与游戏验证如下。

## TMNT 与状态栏的提交快照

- 扩展 probe 对应 APK 的 0.75 运行：PID 14657 / gen1，实际屋顶移动、镜头响应并触发 Leonardo 对话，`GAMEPLAY_REVIEWED`，正常 UI Stop。日志确认 1920×1080→1440×810，以及 BC1→ASTC 6×6 / BC3→ASTC 4×4。证据见 [final-075](evidence/internal-scale-20260919/final-075)。
- 同版 0.5 运行：PID 17988 / gen1，读取当前存档进入下水道据点。日志包含 1920×1080→960×540、BC mip-drop；观察到角色移动，但首轮 manifest 为 `TIMEOUT_UNVERIFIED`，追加输入响应检查为 `ERROR_UNVERIFIED`（review 超过有效时间），保留原结论，不能追认完整验收。0.75 屋顶和 0.5 据点不是匹配场景，不能直接计算性能收益。
- 最新 APK SHA256 `0de65d437fe607500fc15263418ebfa66d46d65e7a2e37c5f83c85cadf65f843`，host `7895d4cdf264d5091526b3ec3aa80e7a20344da6cc85261a663bae362d9ccc26`，JNI `fe1a34de0eb42efd148df78790dc1f37332d5b1aba88835476bfdeef7b77d5bd`。此版补充状态栏倍率，已编译、安装；[截图](evidence/internal-scale-20260919/overlay-050/frame-0031.png)可见 `Surface 1920 x 1080 (x0.5)`。Surface 是 Android 输出尺寸，括号是当前 session 的 internal scale，来自实际 renderer 配置，不表示所有资源都符合缩放条件。
- 最新版 PID 22176 / gen1 / UUID `9fed4d8d67cae4476094f42d1676fe97`；用户报告 Controller disconnected 后卡住、无声，明确要求后续复现再处理。停止自动输入并保留记录，没有确定原因、修复或重启当前会话；该轮为 `STOPPED_UNVERIFIED`。归档最后截图本身未显示该弹窗，不能用它否定用户报告。
- **尚未完成**最新 APK 的 1.0 对照和稳定性验证，也未证明 0.5 / 1.0 的总内存差值。物理 backing 缩小已有分配与 GPU 回读证据；guest 原始内存、缓存/临时资源和进程/系统总内存需分别统计，不能由倍率推算 RAM 读数。
- 用户随后要求先 commit/push；本次只整理提交，没有继续卡死调查或启动新的设备对比。自动输入已关闭，设备最后使用的全局配置仍为 0.5 / High（1×1）；随后用户明确选择 0.5 为默认值，因此不再恢复为 1.0。

## Settings 与默认倍率

按提交前的最新要求，Android 无覆盖值时采用 0.5，损坏 profile 的回退也采用同一默认常量。Settings → Runtime → GPU → **Internal Scale** 提供 0.5 / 0.75 / 1.0 三档选择，全局与单游戏分别保存；重置单游戏值恢复全局，重置全局值恢复 0.5。已有明确 1.0 / 0.75 的配置继续保留，不强行覆盖。

本次默认值与设置链路定向检查 23 项通过：runtime 17 + SettingsViewModel 6。新增设置检查通过实际 ViewModel / RuntimeProfileStore 切换三档、读回保存值、单游戏覆盖及重置回默认值；没有把 UI 状态当作运行中立即换倍率。APK 构建成功，见 [产物与测试汇总](evidence/internal-scale-20260919/default050-apk-artifacts.json)。当前游戏仍在运行，未为安装这一默认值版本打断它；设备已安装版本含 Internal Scale 入口及倍率状态栏，运行配置本身已是 0.5。

## 当前边界

仅对本轮涉及的配置、GPU 图像/shader/压缩链和 TMNT 做定向验证。未覆盖所有 PS4 游戏、所有 shader 指令组合、所有深度/MSAA/alias 组合，未做完整回归、长期稳定性或 Swan/VR 验收。缩放节省的是可缩放资源的渲染/采样/分配开销，CPU/FEX、guest 同步和不可缩放 pass 仍可能限制 FPS；不能由尺寸平方直接推算整帧加速比。
