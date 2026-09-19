# TMNT 高通系统驱动渲染排查（2026-09-16）

## 范围与状态

用户暂缓 Bloodborne，转为尝试修复 TMNT 的高通系统驱动画面。仅检查已有
RDC 和普通 APK 画面；没有重新 GPU 抓帧，没有跑完整回归，没有切 Turnip。
本轮不能声称渲染已经修好。此前工作区包含大量独立未提交工作，全部保留。

设备：AYN Thor / `9c2841a4` / Android API 33 / 4 KiB / Adreno 740；
系统驱动 512.676.53，Vulkan 1.3.128。Host/JNI 为 RelWithDebInfo，FEX Release，
Android 包装仍为 playstoreDebug。数据、原始输出和每轮普通截图在 `build/qfix/`。

## 已有帧的证据

使用 `build/graphics-tooling-20260916/tmnt-rooftop-writing-timeout.rdc`，
1167676491 bytes，SHA-256：
`4e1e908e4a1ecb1cdba9c16ed7ad34bb5701bc200904795911b2e8a32b8bd455`。
这是已有有效抓帧，文件名的 timeout 不能当作本轮抓帧或 replay 失败。

追踪到的资源链：

- guest 输出 387，经 host event 4716 绘制；其场景来自 6656。
- 6656 在 event 3834 读取 HDR 场景 8806；雾效之前已有异常边缘。
- 雾效 draw 4179、4278、4373 采样 R32F 深度副本 10232。
- 10232 由 event 3356 从 D32SfloatS8Uint 深度 8784 采样得到。
  原始 guest VA 分别为 `0x27f3d0000`、`0x27e080000`。
- 用等价、简单 GLSL 替换 event 3356 的 pixel shader 10225 后，
  event 4716/resource 387 的输出 PNG 与恢复后的 PNG 完全相同：
  SHA-256 `a4cf5d0ab0c3fd87f8919107fe9287992460263cf7e7ddb0a950f77152dd001c`。
  这只排除了这个具体替换作为有效修复，不证明所有采样路径都正确。
- 12 个非远平面位置的原深度 `pixel_pick` 与 R32F 副本取样一致。
  10232 的 64 像素间隔全屏网格中，值主要为 1，最小 0.9790017605。
  此现象是线索，不能仅据此断言所有等于 1 的像素本应有几何覆盖。

进一步对同一帧的 838、2000、3180、3338 取样：若干屋顶/墙面候选位置持续
为 1；例如 `(832,704)` 在后两阶段变为 0.97900176，`(1408,896)` 变为
0.98070085。原始请求/结果在 `rp/d4-*.json`。

## 状态读取与工具限制

现有 `pipeline_get` 仅声明 `nativeStateAvailable=true`，不提供实际深度状态。
`pipeline_outputs` 优先读取 action metadata，viewport 的零值是未知，不能当成
真实零 viewport。用配对 RenderDoc 本地 native 源码的临时日志读取
`GetVulkanPipelineState()`，未改变 Android 抓帧/replay 库或公共 ABI：

- 838、1000、1500、2000、2500、3000、3180：深度测试/写入开启，
  `CompareFunction::Less`，bounds/stencil/bias 关闭。
- viewport `(0,1080,1920,-1080)`，深度范围 `[0,1]`。
- `depthClampEnable=1`、`depthClipEnable=1`。
- 深度复制 3356 的测试/写入关闭；雾效 4179 测试开、写入关。

日志在 `rp5/mcp-server.log`。临时 native 源码与 dylib 均按操作前逐字节备份恢复，
没有发布工具新版本。读取的是已有帧，不是下面每个新 APK 的状态抓帧。

**Pixel history 不可用：** 本轮调用触发 Android RenderDoc replay 进程 native crash，
栈在 `BindDescriptorSetsForPipeline → BindPipeline → ReplayDrawWithQuery → PixelHistory`。
这是 replay 工具崩溃，不是 TMNT 崩溃。之后的空 modifications / zero rows 不构成
“没有写入”证据，已弃用。D32S8 `texture_region` 返回 unsupported；数值结论使用
`pixel_pick`，不把 unsupported 的空数组当作零深度。

## 已验证的负例

下列开关分别测试，不叠加；都到普通 APK 的 TMNT 屋顶实景，块状边缘/窗户错误
仍存在，随后撤回。主菜单、loading 或 warmup 的 STOPPED_UNVERIFIED 不算画面验收。

| 试验 | 结果/限制 |
| --- | --- |
| 禁用高通动态 vertex input，走现有静态路径 | 无改善，`static-now.png` |
| 仅关闭 shader float-control execution modes | 无改善，`float-now.png`；不是禁用 FP16 |
| EndRendering 后补 attachment-write → all-commands read/write barrier | 无改善，`barrier-now.png`；不能排除其他同步错误 |
| 高通开启现有 LDS barrier pass | 无改善，`lds-now.png`；不保留额外同步 |
| 顶点 Position 加 Invariant | 无改善，`invariant-now.png`；后来核实主要 draw 是 Less，并非 EQUAL |
| 仅在高通且裁剪开启时关闭 depthClamp | 无改善，`clamp-now.png`；不保留语义变化 |

Citron 的位置输入 workaround 只针对非 fragment Position 输入；其外部 render-pass
依赖 workaround 针对 GENERAL 下 Store→Load。shadPS4 当前主要 attachment 采用专用
layout，因此没有直接复制这两项，也没有重写为 legacy render pass。

## 纹理转换的独立 GPU 检查

将生产 `tiling.comp` 编成 8/16/32/64 bpp 两个方向，直接加载系统 Vulkan：
**8/8 PASS，每项比较 1 MiB，MISMATCH=0**。CPU 独立按 Morton / pipe / bank 公式
构造映射，并检查映射无重叠且覆盖完整；不是 tile→detile 自循环比较。

复现材料：`build/qfix/tiling-check.py`、`tiling-probe.cpp`、`tiling/*`、
`tiling-system-results.txt`。覆盖 Thin2DThin、single sample、single mip、8 pipes/16 banks；
不能推广为所有 depth tile split、mip、MSAA、BC 格式或完整 guest shader 都通过。
这属于独立 CLI GPU 检查，不冒充普通 APK 游戏像素验收。

## 后续定位依据

优先沿 8784 的具体几何写入继续：对同一 draw 的预期覆盖、顶点投影后的 Z/W、
深度裁剪/夹取、实际深度存储做对应验证。现有证据不足以断言是某个单独的 barrier、
Int64 lowering、FEX 或雾 shader 错误。不要以增加 CPU 等待、关雾、放宽深度比较或
改全局深度格式掩盖图像问题。

完整原始证据索引：`build/qfix/diagnostic-evidence.json`。最终部署状态见本文件末尾。

## 最终恢复状态

六项试验均无可见修复，全部撤回；五个触及的生产源文件与试验前备份逐字节一致，
校验记录见 `build/qfix/restored-sources.json`。保持原有 Int64/异步提交/标记等工作。
原有未提交代码重新构建并安装（7c2f47ce，正是之前已构建但未部署的 marker 包；
不是最初设备上的 2126d946），设备 APK SHA 已与本地逐一核对。完整清单位于 `build/qfix/final-artifacts.json`。

- APK SHA-256：`7c2f47cedce18cf3527794fce3157a7bd6a846dbe797ca1d203b7acdc4a6b6d2`。
- lib/arm64-v8a/libshadps4_fex_session.so: `d471c459b876b04b123f3fd6281d2292e3425e9a7084f15fd07f96d5fdbcc2c2`。
- lib/arm64-v8a/libshadps4_host.so: `178f48bf090476e48589a20c2a5d1e4c74c40b917f6f15f8f90cd275eee95247`。

最后诊断轮通过实际 UI Stop 达到 `Stopped/user_stop`；自动按键结束，replay/临时 bridge
均关闭，无 debugger 挂接。恢复包安装后不自动重新开始长时间游戏。
只新增本报告与 AGENTS 交接信息，不提交/推送；不宣称画面或性能修复。
