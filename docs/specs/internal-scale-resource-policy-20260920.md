# Internal Scale 改良 spec：资源分流、小贴图保护与动态尺寸

日期：2026-09-20。状态：**推荐方案，尚未实施**。

本轮交付为源码评估和设计，不代表新增 GPU、游戏画质、内存或性能验收。shadPS4 审计基线为 `1db95bffd5e5c6ba509694aad191ea115532582d`；当前低倍率实现、既有回退与异步退休协议继续作为起点。

## 1. 推荐结论

把现有一个 Internal Scale 拆成两个独立策略：

- **Render Scale**：控制可缩放渲染目标及其绘制坐标，保留 0.25 / 0.375 / 0.5 / 0.75 / 1.0，Android 默认仍为 0.5。
- **Texture Quality**：控制来自 guest 上传、以采样为用途的资产纹理；默认 Native，另提供经过验证的 Balanced / Memory Saver。优先保留原压缩格式、按已有 mip 降级，并保护小图；不跟随 Render Scale 的每次变化。

分类依据是**资源用途、内容来源和子资源历史**。“静态 / 动态”仅作为更新成本信息。流式上传的材质仍是资产纹理，上一 pass 写出的 RT 即使下一 pass 只采样，仍属于渲染结果。UI、字体、LUT、深度、storage 等还需要各自的安全限制。

这是 PS4 和 Switch 都有意义的分离：屏幕像素负载与资产纹理细节/驻留成本不同，资源又常在写入、采样、复制之间切换。它不保证 FPS 或进程内存改善，也不等同于在模拟器里重新实现游戏引擎的完整 streaming 系统。

## 2. 当前实现核对

| 已核对事实 | 对改良的影响 |
| --- | --- |
| [Image 构造](../../src/video_core/texture_cache/image.cpp)直接读取一个全局 Internal Scale；`eligible` 仅要求原始宽高均 ≥16，再检查类型、采样数及格式能力 | 16×16 在 0.25 下可以变成 4×4；现有门槛不是缩放后尺寸保护 |
| [InternalScale](../../src/video_core/texture_cache/internal_scale.h)用八分之一倍率；只有 0.25 / 0.5 对应丢两级 / 一级 mip | 0.375 / 0.75 的资产纹理常走重采样，受支持 LDR BC 会进入 ASTC 编码；不应为匹配 RT 倍率而必然重编码 |
| [ImageDesc](../../src/video_core/texture_cache/texture_cache.h)已有 Texture / Storage / RenderTarget / DepthTarget / VideoOut，但构造 Image 时仅传 ImageInfo | 已有用途入口可复用，需要把意图带到分配决策，不能只加两个设置值 |
| [ImageUsageFlags](../../src/video_core/texture_cache/image.cpp)广泛预设 sampled、attachment 和 storage 能力 | VkImage 的 usage bits 不能充当实际用途分类器 |
| [BindResources](../../src/video_core/renderer_vulkan/vk_rasterizer.cpp)已有 exact-access、MSAA、fragment side effects 和混合附件回退 | 必须保留，并改成以整组附件最终决策为准，不能仅检查每张图 `IsScaled()` |
| [shader image lowering](../../src/shader_recompiler/backend/spirv/emit_spirv_image.cpp)已有 guest 尺寸查询、整数 fetch、view-relative mip-drop 映射 | 不应另建一套忽略 guest 逻辑尺寸的纹理系统 |
| [PushData](../../src/shader_recompiler/resource.h)为 128 字节，30 个 unified binding 的两位 image code，另有 render scale | Native / drop1 / resample / drop2 可继续表达第一版；不能把 RT 倍率拿来推断每个 sampler 的倍率 |
| [内存实测](../validation/android-native-host/internal-scale-memory-20260919.md)中缓存图像 allocation 386.85→112.62 MiB，但 VMA blocks 与进程 PSS 未下降；后续测得约210 MiB全尺寸在途上传源 | 降资产分辨率必须同时核算上传临时资源和池保留，不能只比较长期 backing |

既有验证边界见 [五档倍率记录](../validation/android-native-host/internal-scale-low-20260920.md)和[原实现记录](../validation/android-native-host/internal-scale-20260919.md)。这些检查证明已有路径的特定行为，不证明此方案已经生效。

Switch 对照使用干净的本地 Citron `3da08ee52defc5003d23683b0c233b0dac88722f`：

- [ImageInfo](../../../../switch/citron/src/video_core/texture_cache/image_info.cpp)区分 `rescaleable/downscaleable`；纹理描述符路径要求单 mip，使用 288 / 512 的高度启发式。
- [TextureCache](../../../../switch/citron/src/video_core/texture_cache/texture_cache.h)的 `RescaleRenderTargets` 整组检查 color/depth，`ImageCanRescale` 检查别名链。
- 这些是可借鉴的用途分流与一致性边界。不能直接移植高度阈值、断言所有单 mip 都是 RT，或把其策略当作 PS4/Switch 硬件规则；本轮也未验证 Citron 的游戏效果。

## 3. 设置及推荐初值

| 设置 | 第一版建议 | 说明 |
| --- | --- | --- |
| Render Scale | 现有五档；Android 默认 0.5 | 作用于准入的渲染目标；保持重启游戏生效 |
| Texture Quality = Native | **默认**；额外 mip drop=0 | 不因 Render Scale 缩资产图；guest 自身 streaming/mip 选择照常 |
| Texture Quality = Balanced | 最多额外丢1级；保留首级短边至少128 | 只对准入且有有效 mip 的资产启用 |
| Texture Quality = Memory Saver | 最多额外丢2级；保留首级短边至少64 | 明确的额外画质取舍；仍受同样的语义保护 |
| 单 mip / mip 不足的资产 | 默认保持原尺寸 | 第一版不自动改成每次更新都重采样/ASTC 编码 |
| 可选资产重采样/转码 | 后续单独准入，初始关闭 | 复用现有 Foundation 路径，独立验证画质、峰值与更新成本 |

64 / 128 是建议起始策略参数，需通过资源直方图和画质验证校准，并非硬件要求。先作为内部 preset 参数，避免设置页出现多组阈值滑杆。

旧 `internal_scale` 值继续决定 Render Scale；新增 Texture Quality 未配置时采用 Native，迁移说明需明确“资产纹理恢复原质量，长期纹理占用可能增大”。保留旧字段读取兼容；新字段支持全局/单游戏继承、重置和配置往返。此 spec 不修改用户当前设置。旧版本耦合策略只保留在开发对照开关中，不作为新增公开默认。

Native 的选择是首版兼容性建议。若设备内存预算要求默认 Balanced，必须先完成下述内存和场景验收，再做产品默认变更，不能把减少 ASTC 临时资源推导成净内存一定下降。

## 4. 用途模型与内容所有权

建议引入轻量 `ResourceScalePlan`，由 TextureCache 决策，Image 执行。至少包含：

```text
identity: session + guest mapping generation + image identity + subresource range
usage_history: sampled / attachment / storage / transfer / readback / video-out
content_origin: guest-upload / render-output / compute-output / copy / unknown
policy_domain: asset / render / native-required
logical_layout: guest extent, pitch, mip layout, layers, samples, format
physical_plan: host extent, format, mip mapping, effective render scale
content_version, plan_version, reason, optional source-family identity
```

不建立两个互不相认的 TextureCache。guest 地址、别名、dirty tracking 和内容版本仍由现有统一 cache 管理；资产与 RT 的分配策略分开。第一版可对整 Image 保守取各子资源限制的并集，避免为不同 mip/layer 建复杂的多 backing 内容协议。

| 观察到的使用 | 推荐处理 |
| --- | --- |
| guest 上传的只采样材质，含后续 streaming 更新 | Asset；可按 Texture Quality 选择已有 mip |
| color/depth attachment，随后被 sampler 读取 | Render；继承写入时 backing，采样时不再执行 Texture Quality |
| RT 复制到另一个普通纹理描述符 | 传播内容来源和尺寸映射；不能将复制结果误当 guest 资产再缩一次 |
| 已确认字体、UI atlas、LUT、查表/精确数据纹理 | NativeRequired；保护与原图大小无关 |
| writable storage、atomic、整数数据、精确 offset/LOD query、未支持的 copy/alias/MSAA | 保留现有 native 规则；不自动缩 compute dispatch |
| 用途或内容来源不明 | 原尺寸；记录原因，后续用明确证据扩大准入 |

“多帧没有被写”不能证明静态资产；“刚被 CPU 更新”不能证明资产；BC 不能单独证明是可降质材质。资源地址重用、新映射和不兼容描述符必须更新 identity，不能沿用旧的“静态/可缩放”标签。

角色改变时，在新用途的 descriptor/pass 发布前重新决策，并从**最新内容**构造所需 backing。已经 GPU 修改的内容不得用旧 guest RAM 重建。第一版遇到无法无歧义转换的 mixed usage，单向回到 native；不得在每次 sampled↔attachment 切换时反复缩放。

需要明确一个现有限制：**把低分辨率 RT blit 回原尺寸不能恢复已经丢掉的像素或精确数据。** 因此 exact/readback-sensitive 资源必须尽量在首次有损写入前排除；后发现的 `ForceNative` 只是停止后续损失，不能被描述成 bit-exact 修复。要求精确 guest 数据的路径在无法事先准入时应保持 native，并将晚发现情况列为兼容性缺口，而不是用放大图冒充原始结果。

## 5. 小资产纹理保护

保护对象是**当前有效最高分辨率内容的首级**，同时覆盖很窄的长条图；不能只检查面积，也不能只检查 canonical 原图曾经有多大。

有有效 mip 链时，从不超过 preset 上限的候选 drop 中选择最大合法值：

```text
d = largest integer in [0, max_extra_drop] such that
    retained guest mip is resident, initialized and sampleable
    retained chain can implement the guest-visible sampler/view range
    max(1, W >> d) >= min_retained_edge
    max(1, H >> d) >= min_retained_edge

if no positive d qualifies: d = 0
```

先执行语义保护，再计算尺寸。这里的 `W/H` 是可确认的当前内容尺寸，不能把对齐 pitch 当宽度。**最低边长只约束被保留的首级，不能把 mip tail 的 4×4、2×2、1×1 全部钳成64或128。** 小于阈值的原图保留原尺寸，不进行放大。数组 layer 数、cube face 数与 volume 深度不随2D质量档下降。

| 当前资产尺寸及有效 mip | Balanced | Memory Saver | 原因 |
| --- | --- | --- | --- |
| 16×16、32×32、64×64 | 原尺寸 | 原尺寸 | 正面解决极低倍率下小图继续缩小的问题 |
| 128×128 | 原尺寸 | 64×64（drop1） | 取合法候选，不能强行drop2 |
| 256×256 | 128×128 | 64×64 | 与 Render Scale 无关 |
| 512×512 | 256×256 | 128×128 | 不超过各档最大 drop |
| 1024×32 | 原尺寸 | 原尺寸 | 短边保护，面积门槛无法替代 |
| 65×49 | 原尺寸 | 原尺寸 | 不再因原尺寸≥16而进入有损缩放 |
| 2048×2048，单 mip | 原尺寸 | 原尺寸 | 缺少可直接使用的低 mip，初版不自动转码 |
| 任意尺寸的已识别 UI/LUT/精确数据 | 原尺寸 | 原尺寸 | 语义保护优先于尺寸 |

大小规则不能识别大字体 atlas 中的小字形，也不能辨认所有 normal/roughness/alpha-test 内容。缺少可靠语义证据时，默认 Native 才能避免普遍降质；Balanced 只作保守准入的可选策略。已有作者制作的 mip 优先于运行时重采样，但仍要验收透明边缘、法线与材质细节，不能因此声称无画质损失。

若后续允许无 mip 重采样，应使用一个保留长宽比的倍率，并检查缩后两边、真实压缩块数及收益。禁止分别 `max(width*s,64)` / `max(height*s,64)` 拉伸比例；sRGB 在线性域过滤，法线、alpha coverage 和数据图没有专门正确处理时维持 native。

## 6. Streaming LOD 与 guest 采样语义

guest 可能通过 view base mip、替换更小纹理、更新部分 mip、复制或地址重用实施 streaming。模拟器只有在描述符/上传/别名关系能证明时，才知道两个版本属于同一资产。不能从连续地址或相似尺寸猜测跨对象的源资产身份。

对于能确认 canonical mip 身份的资源，按绝对 mip 目标限质，而不是每次在 guest 当前级别上重复 drop：

```text
guest_first = guest 当前有效首级在 canonical 链中的 mip
policy_first = Texture Quality 为此 canonical 资产决定的最低保留 mip
host_first = max(guest_first, policy_first)
extra_drop = host_first - guest_first
```

然后再施加小图下限、有效子资源及可采样范围检查。例：4096 原图，Memory Saver 的绝对目标为 mip2 / 1024；guest 当前在 mip1 / 2048，则额外丢1级；guest 已在 mip3 / 512，则额外丢0级，不能再变成128。已知 canonical 身份断开时，不能套用此例。

检测到资源尺寸/mip范围变化却无法确认 canonical 身份，第一版停止额外降质，保留 guest 当前尺寸。普通 dirty 更新只触发受影响内容刷新，不自行改变质量档；部分上传没有触及某级不等于该级不存在，也不能访问从未初始化的 mip。若原有 guest 采样已合法，新增策略必须保证其所有访问仍有定义的映射；无法证明时回退原路径。

新增策略必须分别验证：

- guest `textureSize`、level count 等逻辑查询保持原描述符语义，view base、sampler min/max LOD 与显式 LOD 统一映射；不能直接泄露更小 VkImage 的尺寸。
- drop `d` 时显式 LOD 按 view-relative 的剩余 drop 转换；base mip 已越过被丢区间时不得再减一次。缺失高分辨率 mip 的视觉损失是质量选择，不是精确等价。
- 隐式 LOD/显式梯度使用实际 host 尺寸产生物理 footprint；不得再无条件追加 `log2(RenderScale)` 或重复 mip bias。理想同视角下，导数随低分辨率 RT 增大，**保留原纹理并不意味着每次都采样最高 mip**。
- 对于归一化坐标、等比缩放的理想情形，物理 LOD 相对原生参考约变化 `log2(texture_factor / render_factor)`；只用于推导检查，不替代对偏移、gather、非等比/奇数尺寸和原生 shader 语义的测试。
- integer fetch、gather/offset、LOD query、深度比较、超出30个可编码 binding 等继续采用已验证映射或 native 回退。两个设置独立后，Render Scale=1、Texture Quality≠Native 也必须开启相关 shader 处理，反向组合亦然。

第一版不实现基于反馈的 host 自动纹理 streaming、磁盘资产缓存或动态显存预算控制器。之后若加入这些能力，必须有内容版本、请求取消、滞回和预算，不能与游戏原 streaming 互相追逐。

## 7. RenderTarget、小型 pass 与 dynamic size

### 7.1 RT 不能直接套资产图的64/128门槛

RT 有主场景、G-buffer/depth、阴影、反射、UI、bloom/曝光金字塔、时序历史等不同角色。它们可能是单 mip，也可能被当纹理采样。保护小材质不应让全部低分辨率后处理失去原有比例。

第一版按整组附件做确定性决策：

1. 先合并 color/depth 的已知用途和现有安全限制。所有附件必须使用同一坐标变换；“都不是native”不代表实际倍率一致。
2. 未分类且短边≤64的小 RT 先保留native；该值为初始启发式，不宣称识别其用途。已确认屏幕相对的后处理链可继承 render 策略，不套资产首级下限；允许合法的1×1尾端。
3. 任一必须native的附件，或无法支持的一组尺寸/采样数组合，使该 pass 整体native。不要将一张附件单独抬到64/128，另一张仍乘0.25。
4. 小 RT 与大附件共用一个 pass 时，上述保守回退可能损失性能，诊断必须可见；后续以明确的 pass/资源证据解除，不按游戏名硬编码。
5. standalone 阴影/UI/历史缓冲不能仅凭面积或宽高比自动认作主场景。无法确认其安全缩放规则时保留native，允许后续逐类扩大覆盖。

UI 资产原尺寸不能保证最终文字清晰：若文字仍绘制到0.25的场景 RT，输出依然受低分辨率限制。只有 guest 本来有可区分的 UI 合成阶段时，才可以在该阶段保持native；第一版不宣称通用地拆出所有游戏的 HUD。

### 7.2 区分三种“动态”

| 动态行为 | 第一版要求 |
| --- | --- |
| guest 重新创建/复用不同 extent 的 target | 使用本次真实逻辑 extent 重新算 host backing；正确区分 mapping/content/plan generation |
| guest 分配固定最大 RT，只改变 viewport/render area | backing 按分配 extent 缩放，active rect 单独转换；不能拿 active rect 冒充allocation并改掉UV归一化基准 |
| emulator 为追逐帧率主动改变 Render Scale | 本版不启用；保留重启生效，另行设计控制器与历史缓冲处理 |

设本次 guest allocation 为 `G=(W,H)`，通过准入的 session render factor 为 `r`：

```text
host allocation = max(1, floor(G * r))  // 分量计算；仅逻辑extent，不改guest pitch
host viewport / active rect = 按同一render映射转换
source mip / sampled image extent = 使用该资源自己的plan
```

保留当前负 viewport 高度、深度/fog修复；scissor 起点floor、终点ceil后裁到合法host范围。奇数尺寸需要通过GPU边缘像素验证，不能用名义倍率替代真实 mip extent 的 fetch/copy 映射。

配置语义明确为 **Relative to guest**：游戏从1920×1080降到1280×720，r=0.5时host从960×540变为640×360。这种相乘是用户选择的含义，应在诊断中同时显示guest active尺寸与host尺寸。只乘一次；RT在后处理被采样时不得再乘Texture Quality或Render Scale。

如果以后需要“总输出最多960×540、但不在guest已经降得很低时继续砍半”，应另做 **Absolute ceiling** 策略；不能暗中把Relative语义改为追随显示Surface或固定1080p。极低倍率下的整体可读性与小资产保护是两个独立问题。

已有guest DRS切换必须保留游戏自己的内容、清除和历史有效性语义，不能擅自清空TAA历史。之后如实现host动态倍率，必须能处理历史重投影、jitter和motion vectors；无法正确处理的history链保持固定分辨率/native。不要每帧销毁重建整个cache，也不要无上限缓存所有曾出现的尺寸。

## 8. 上传、压缩与内存成本

优先次序为：**native压缩采样 → 直接使用既有压缩mip → 有证据收益的重采样/转码**。RT继续使用合适的attachment格式，不做常规每帧BC→ASTC处理。

Texture Quality 切换与 guest dirty 是不同事件。内容未变、只是 RT 尺寸/绑定改变，资产纹理不得重新 detile/encode。持续更新的纹理若转码开销过高，应停止额外降质或走验证过的直接mip路径，不能因为“动态”就自动进入RT策略。

实际存储估计按每个mip计算压缩块：`ceil(W/block_w) * ceil(H/block_h) * block_bytes * layers`；再分别记录 Vulkan allocation、VMA block 与进程PSS。尺寸平方只是大图理想近似。直接丢mip只有在上游detile/staging也跳过被丢级时才可能减少对应上传工作，不能只凭更小VkImage宣称全链节省。

可选重采样/ASTC路径进入推荐preset前，要求有界scratch、明确在途峰值、格式/颜色空间/alpha验证和创建速率统计。复用必须满足命令顺序与跨队列依赖，资源退役仍要求GPU完成和host submit返回；不得加device/queue idle压低内存数字。复用池用尽时采用有预算的原生路径/既有可取消背压，不无限增建临时源，也不回用仍在读取的scratch。

## 9. 推荐实施范围与文件入口

按两个连续工作包落地；每包完成代码、针对性验证和证据，不逐个开关交回微型spec。

**WP1：独立策略及正确性闭环。** 配置分离；BindingType/usage history传入分配；Asset mip保护；RT→sample/copy来源传播；streaming身份与dirty；RT动态extent/active rect；shader两策略组合；别名与生命周期回退；资源诊断。第一版Texture默认Native，Balanced/Memory Saver仅在相应准入检查通过后开放。

**WP2：成本与真实场景闭环。** 复核保留的上传路径与峰值；定向GPU probe；两个游戏同场景画质/内存/性能对照；必要时收紧preset与默认。scratch池/融合detile-scale是成本优化候选，不要求为了形式统一重写Foundation或所有texture cache。

| 入口 | 改动职责 |
| --- | --- |
| Android配置/JNI、native settings | 两个独立配置、迁移、session快照、继承/重置；不在UI堆策略细节 |
| `texture_cache/internal_scale.h` | 纯尺寸与mip规划；不再隐含“所有图像同一倍率” |
| `texture_cache/texture_cache.h/.cpp` | 用途证据、共享identity/alias、最终plan、内容版本和dirty传播 |
| `texture_cache/image.h/.cpp`、`image_view.cpp`、`sampler.cpp` | 分配、子资源映射、内容转换、有效view与sampler、异步退休 |
| `renderer_vulkan/vk_rasterizer.cpp` | 整组附件决策、viewport/scissor/render area、最终倍率发布 |
| `shader_recompiler/resource.h`及image lowering | per-binding plan与guest语义；Render=1但Texture降级也生效；需要时升级shader/cache版本 |
| `tests/video_core/android_internal_scale_probe.cpp` | 扩展现有生产GPU probe，避免另写只验证公式的替代实现 |
| `gpu_memory`诊断 | 按用途/原因分组，列出logical/physical尺寸、有效drop、上传和在途成本 |

Foundation继续只承载通用texture codec/Vulkan工具，不包含PS4/Switch guest地址、描述符、游戏名、alias身份或streaming启发式。

## 10. 验收要求

| 验收组 | 必须验证的行为 |
| --- | --- |
| 配置 | 五档Render×三档Texture；旧配置迁移；单游戏继承/重置；启动与status一致；Render=1/Texture降级及Render降低/Texture=Native两种关键组合 |
| 小图与mip | 8/16/32/64/128/256/512、65×49、长条图、单mip/不足mip、非零view base、数组层；首级下限正确且tail未被抬高 |
| 语义保护 | 大型字体atlas、LUT、integer/storage/atomic、精确offset/LOD query、MSAA；未准入路径保持native且有原因 |
| 用途转换 | Asset→RT、RT→sample、RT→copy→sample、depth→sample、GPU写后CPU读、地址重用、别名、部分mip更新；无双重缩小、旧RAM覆盖或stale view |
| streaming | 已知canonical链的guest mip0→1→3→1；未知身份的更小重建；有效/无效mip与部分dirty；无重复drop或未初始化访问 |
| dynamic target | allocation变化与固定allocation/active rect变化各测；1920×1080/1600×900/1280×720往返，含奇数尺寸、MRT/depth混合和history内容检查 |
| shader/GPU | 五档、两个质量策略组合、隐式/显式LOD/Grad/fetch/query、view与sampler范围、binding上限、安全fallback；Qualcomm与Turnip真实GPU回读及SPIR-V验证 |
| 生命周期 | in-flight旧view/backing、延迟submit返回、Stop/取消；无提前回收、无新增全局idle、预算不无限增长 |
| 真实场景 | TMNT、Bloodborne中实际可操作场景；HUD/字幕/图标、近距材质、移动/镜头、透明边缘、阴影/反射/后处理；guest streaming/DRS未触发的项目标为未覆盖 |

对照最少包含：原版本耦合策略、Render相同+Texture Native、Render相同+Balanced；极端Render=0.25/0.375增加小图/UI审查。固定APK/驱动/存档/相机/预热，分别测GPU时间、呈现帧时间P50/P95、CPU上传/编码成本、缓存allocation、scratch峰值、VMA blocks和PSS。Render=1/Texture Native作为原质量参考。跨视角或跨场景数字不得直接归因于策略。

诊断至少能解释“为什么某张图没有缩小/为什么某个pass回到native”，并区分尺寸保护、数据语义、未知用途、alias、mip不足、更新成本与budget。默认不逐draw打印；按请求输出聚合与有界top-N，绑定session和采样时刻。

验收分别报告 **语义检查、画质观察、内存、CPU/GPU成本、呈现性能**；不能用数百万算术/回读检查代替游戏可读性，也不能用缓存allocation下降代替总内存下降。本轮不要求完整游戏回归；新增质量阈值和所有未覆盖角色必须保留边界。

## 11. 设计依据

- [Khronos：VkImageUsageFlagBits](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageUsageFlagBits.html)：sampled与attachment是可组合的用途能力，不能将Image当成永久互斥的两种类型。
- [Khronos：Images / SPIR-V Image Query Instructions](https://docs.vulkan.org/spec/latest/chapters/images.html)：image view base mip与level count影响尺寸/级数查询，因此物理mip裁剪必须维护guest逻辑映射。
- [Epic：Texture Streaming Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/texture-streaming-overview-for-unreal-engine)：引擎streaming依赖可见性、纹理需求和预算；模拟器通常不具有等价的场景语义。本方案据此采用保守的host资源策略，而不声称重建原streamer。
- [Epic：Dynamic Resolution](https://dev.epicgames.com/documentation/en-us/unreal-engine/dynamic-resolution-in-unreal-engine)：动态分辨率涉及view、缓冲尺寸和时序历史；这支持把guest动态尺寸兼容与host自动调节分开设计。该资料是设计参照，不代表两款测试游戏都采用该实现。

上述来源支撑通用语义；preset、阈值、迁移方式和实施范围是本spec的工程建议，需由本仓实现与验证确认。
