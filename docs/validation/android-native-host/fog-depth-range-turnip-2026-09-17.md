# TMNT 残余雾硬边：新 RDC、Turnip 源码与 viewport 深度映射

本轮是定位与回放诊断，**没有把诊断 shader 部署到 APK，也没有声称雾效已正式修复**。
之前 Qualcomm 三条动态 depth-state 重发仍只解决严重的栏杆/窗户破面。

## 主要发现

雾硬边在 Turnip 也存在，已经定位到其 RDC 的 **event4397**：
`fs_0x00000000d63cd69e_0` / `vs_0x00000000c9f45533_0`，pipeline11803，
1512 indices、1 instance、504 triangles，输出 RGBA8 resource7014。
前一 draw4388 增加雨线；4397 才叠加大片硬边雾。
marker 的 PM4 packet VA 为 `0x2397445fc`，submission4238、guest_flip1412。
这些 ID 仅属于该 Turnip capture，不能用于新 Qualcomm capture。

目前最强线索是 **shadPS4 对受限 Vulkan depth range 的截断改变了深度映射**，
让雾 shader 的线性深度还原与顶点 view-space depth 不在同一尺度。
回放中的 shader 替换、单片元 trace、实际 Vulkan API 参数及 Turnip 寄存器生成公式互相吻合。
尚未直接记录当时 guest 的 viewport zscale/zoffset，不能将所有游戏、所有雾效问题都归到此处。

## 新 Qualcomm 抓帧及深度复验

设备 AYN Thor `9c2841a4` / API33 / 4KiB / Adreno740；APK5884b68f 未更换。
Host/JNI RelWithDebInfo、FEX Release；系统 Qualcomm512.676.53/build69e13475cb。
PID31460 / generation1 / UUID `74ea8b9cb84414149f762aef037c6619`。

- 新文件：`build/fog-compare-20260917/tmnt-rooftop-qualcomm-fixed.rdc`
- 1,110,985,693 bytes，SHA256 `34b56a436e7820916800c8060f093a0d0d8d9442ad350399a1bfe19668cd7c52`。
- Ready，cleanup_pending=false，guest_flip2152→2153；sidecar 与主机文件一致。
- 对应系统 ICD 的最终回放成功；resource397 / host-sampling event5856。
- R32F depth copy resource9773、event5856、mip0/slice0/sample0：1920×1080，每64像素取样，510点。

| 同一采样规则 | 旧 Qualcomm | 修复后 Qualcomm | Turnip |
| --- | ---: | ---: | ---: |
| 清屏深度1.0的点数 | 498/510 | **0/510** | 0/510 |
| 最小值 | 0.9790017605 | 0.9590414166 | 0.9590413570 |
| 最大值 | 1.0 | 0.9872899055 | 0.9872899055 |
| 平均值 | 0.9995602108 | 0.9795849309 | 0.9795854785 |

这补齐了旧报告未完成的“修复后 depth 数值 RDC 复验”。不同帧的动画/VA不同，
不作逐像素等价或性能比较；覆盖恢复并不代表深度数值的语义已经正确。

## Turnip 的特殊状态处理

本地只读参考：`/Users/bytedance/workspace/bug_reports/references/mesa_turnip/`。
README 标明 commit `903957fea71fdf88c288ecd0b86bb43be711aecc`；
设备 driver git 为 `5ac41be677`，**不是精确相同版本**。
文件 SHA 见 `build/fog-compare-20260917/turnip-source-identity.json`。

1. `tu_cmd_buffer.cc:5509`：绑定 graphics pipeline 时，清除 `static_state_mask`
   对应的 dynamic-state set 位。源码明确解释：静态预编译状态伪装成动态状态后，
   公共框架仍记着更早的真实动态值；若不失效，同值更新可能被错误跳过。
   这是实在的防陈旧状态机制，但不是 Qualcomm 闭源驱动内部缺陷的证明。
2. `tu_cmd_buffer.cc:5429,8486`：fragment shader 改变以及 depth test/write/compare、
   stencil、alpha-to-coverage、feedback loop 变化会触发 LRZ 与 depth-plane 状态重建。
   `:7998` 的 early/late-Z 选择还考虑 discard、sample mask、FragDepth 和 query。
3. `tu_cmd_buffer.cc:5553`、`tu_pipeline.cc:4307`：结合实际 depth/stencil attachment
   和 subpass 重算寄存器；不能只在 bind 时根据 pipeline 静态值处理。
   `tu_pipeline.cc:4088` 专门保留 LRZ/附件重发所需的 depth/stencil 状态。
4. **与残余雾直接相关**：`tu_pipeline.cc:2591` 的 viewport 转换为：

   ```text
   negativeOneToOne = true:
     ZSCALE  = (maxDepth - minDepth) / 2
     ZOFFSET = (minDepth + maxDepth) / 2
   false:
     ZSCALE  = maxDepth - minDepth
     ZOFFSET = minDepth
   ```

上述都属于驱动生成/维护 GPU 状态，不支持增加 queue idle、全局 flush 或 CPU 等待。
Turnip 没有替应用恢复已经被截断的 guest viewport 含义。

## 实际捕获的 API 参数

使用配对 `renderdoccmd convert -c xml` 读取结构化命令，**没有在 macOS 回放 Vulkan**。
导出：`build/fog-compare-20260917/turnip-commands.xml`；精简提取 `fog-api-state.json`。
按 marker、同一 commandBuffer4911 的最近状态和 pipeline11803 创建参数对齐，得到：

- `negativeOneToOne=1`
- viewport `(0,1080,1920,-1080)`，`minDepth=0,maxDepth=1`
- depth test=1，write=0，compare=LESS
- depthClampEnable=1，depthClipEnable=1
- blend=SRC_ALPHA / ONE_MINUS_SRC_ALPHA，ADD
- 设备创建启用 depth_clip_control / depth_clip_enable；未启用 depth_range_unrestricted。

因此这一 draw 的硬件 viewport 明确会执行 `depth = 0.5*(clipZ/clipW)+0.5`。
对应 shadPS4 `vk_rasterizer.cpp:1238–1250`：guest MinusWToW 本应构造
`[zoffset-zscale,zoffset+zscale]`，不支持 unrestricted 时却直接限制端点到 `[0,1]`。
`vk_graphics_pipeline.cpp:132` 仍保留 negativeOneToOne，因而截断会改变缩放/偏移，
而不只是让越界深度合法。

## 雾 shader 的定量证据

4397 的 PS binding1 读场景深度10507；binding2/3 都读256×256纹理11804。
后者是连续噪声/径向遮罩，实际 MCP image block 已检查；这不排除所有纹理问题。
shader 用四个 SSBO 常数还原深度，再乘入软粒子交界淡出因子：

```text
scene = -(a*d + c) / (b*d + e)
soft  = clamp(abs(viewZ/viewW + scene) * 2/3, 0, 1)
alpha = mask * opacity * smoothNoise * soft * 0.5
```

为拆分因素，在同一 replay 临时以 GLSL 重写这一 PS，保留原绑定、varyings 和公式。
这是诊断对照，不是已验证的通用 guest recompilation：常量/浮点模式可能有差异。
但全幅1920×1080输出与原 shader 仅4个像素不同，最大差1/255，
平均RGB code差0.000001286。原 shader 声明 DenormFlushToZero，原始 debugger 拒绝；
这里调试的是明确标注的 GLSL 诊断替换，没有删除原 shader capability 来冒充原始 trace。

在像素(1320,365)、primitive94，诊断 trace 读到：

| 值 | 原公式对照 | 仅把采样深度改为 `2*d-1` |
| --- | ---: | ---: |
| 原始采样d | 0.9792394 | 0.9792394 |
| 用于还原的d | 0.9792394 | 0.9584788 |
| a / b / c / e | -0 / -0.66566664 / -0.99999994 / 0.6666667 | 相同 |
| 雾的viewZ/viewW | -34.12475 | -34.12475 |
| 还原scene | **67.47783** | **34.917034** |
| 距离差绝对值 | 33.35308 | 0.7922859 |
| soft | **1** | **0.5281906** |

同一片元的 projected=(12.814516,11.0273695,32.67376,34.12475)；
pixel history 的该 primitive depth≈0.97874，也与上述半区间 viewport 映射吻合。
Pixel history 的逐 primitive post-color 有复用/合并现象，不拿这些中间颜色证明 blend 正确；
输出判断使用实际 texture review。

其他回放对照：

- 去掉 soft 因子，与 GLSL 对照 PNG 完全相同。
- 把投影坐标采样改为 FragCoord/屏幕尺寸，与 GLSL 对照也相同。
- 可视化 soft：可见雾覆盖区域为饱和1。
- 仅撤销深度的半区间映射：大片直线/多边形硬边明显减弱，软交界恢复。
  **这只是因果诊断，不是应直接部署的游戏 shader 特判**；没有据此声明所有雾正确。
- 每个替换均恢复；恢复原 shader 后的 PNG 与最初输出 SHA 完全一致。

## 正式修复边界

修复应在 guest viewport→Vulkan depth 的兼容路径保持原缩放/偏移，以及 clipping/clamping 语义。
不能全局把所有深度纹理改成 `2*d-1`，也不能只关闭 depth test、忽略 min/max 合法范围，
或为 TMNT 硬编码 shader hash。
需要覆盖 MinusWToW/ZeroToW、普通/反向深度、近远裁剪、深度 clamp、FragDepth 写入和
采样深度的消费者；先针对这一数学映射做小规模验证，再回到普通 APK 屋顶。
本轮没有修改生产 renderer、FEX、Foundation 或驱动；不涉及新的 spec/完整回归/commit。

## 工具与现场清理

新 Qualcomm RDC 的最终输出和 depth-grid 成功，但部分逐事件 replay 在系统 ICD
`vkBeginCommandBuffer` 内 SIGSEGV。临时尝试 eReplay_Full 仍出现该问题；不是游戏崩溃，
也没有把失败的中间结果记为通过。Turnip 分析使用原配对工具；空闲连接曾因25s超时失效，
相关失效连接 pixel_pick 的0值不作深度证据，重新打开后的真实 readback 才用于分析。

- 临时 RenderDoc native 源码/dylib 已 SHA 验证恢复，`tool-restoration.json`/`cleanup.json`。
- 独立 `fog.rdc.debug.db` 保存6个替换记录及2条诊断 shader trace；关闭时
  activeReplacements=0、releaseFailedReplacements=0；capture/debug session 均关闭。
- 5项 capture layer settings、replay loader property 全部恢复前值；GPU Reshape/debugger 均未挂接。
- 系统驱动保持原选择；游戏普通 APK 重启状态另见 `final-device.json`。
- 原始证据、截图、GLSL 对照、XML/API摘要、trace 在 `build/fog-compare-20260917/`。

