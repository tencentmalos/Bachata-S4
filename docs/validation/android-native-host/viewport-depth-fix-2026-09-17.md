# TMNT 雾硬边：viewport 深度转换修复（2026-09-17）

本轮直接修复生产 renderer，保留此前 Qualcomm pipeline bind 后的动态 depth 状态重发。没有修改 TMNT 游戏内容、雾片元 shader、FEX、Foundation、驱动或队列同步。

## 原因与修复

上一轮的 RDC 诊断见 [原始分析](fog-depth-range-turnip-2026-09-17.md)。本轮增加了有次数上限的临时日志，真实 guest 寄存器确实交替出现：

- `clip_space=MinusWToW, zscale=1, zoffset=0, zmin=0, zmax=1`；
- `clip_space=MinusWToW, zscale=0.5, zoffset=0.5, zmin=0, zmax=1`。

前者推导出的 Vulkan viewport 端点为 `[-1,1]`。原代码在没有 `VK_EXT_depth_range_unrestricted` 时直接截断为 `[0,1]`，于是原本的 `depth=z/w` 变成 `depth=0.5*z/w+0.5`。这是 host 转换丢失了 guest 的仿射关系，不能靠重发 depth state 或增加 barrier 修好。日志已经从最终生产代码移除。

新路径只在设备不支持 unrestricted depth range、且活动 viewport 超出 `[0,1]` 时启用：

1. `amdgpu/depth_range.h` 分开保存 guest 深度变换、原始裁剪空间和最终钳制范围。
2. 最终顶点输出将 guest 深度变换放入齐次 `Position.z`，Vulkan viewport 使用合法范围；viewport clamp 使用 guest `zmin/zmax`。处理反向 scale、恒定范围和不同 viewport，参数进入 shader specialization。
3. 原始 `z/w` 的近远裁剪分别输出到空闲 ClipDistance 槽；关闭对**变换后位置**的硬件 Z 裁剪。使用固定功能 primitive clipping，不增加 fragment discard、FragDepth 写入或 CPU/GPU 等待。
4. VS/TES 末尾、GS EmitVertex，以及 rect/quad 辅助 tessellation 的裁剪数据传递已连接。GS 发射后恢复原始 Position，避免后续发射重复变换。
5. Shader binary version 升为 7，shader metadata x86/ARM 分别为 6/7，pipeline key 为 4，避免读取旧深度语义的缓存。

深度钳制发生在片元深度比较前，`FragCoord.z` 不等于已钳制的 depth attachment。探针分别检查两者。[Vulkan 规范](https://github.khronos.org/Vulkan-Site/spec/latest/chapters/fragops.html)

## Qualcomm 的额外兼容问题

有效 SPIR-V 将 Position 和 ClipDistance 声明为独立 builtin 输出时，系统驱动在该路径未执行用户裁剪；Turnip 执行正确。移除数组 initializer、显式写所有槽、在 FS 声明或读取 ClipDistance 均没有解决。

最终使用同一个 `gl_PerVertex` Block 的两个 builtin 成员，并在 shader prologue 中建立访问指针。相同探针的近裁剪、远裁剪、部分三角形裁剪全部恢复。最终 FS 不读取 ClipDistance，不靠片元阶段丢弃修饰结果。这证明了可用的接口表达兼容方法，**没有证明闭源驱动内部某个 cache 或 compiler pass 的具体错误**。

Turnip 本地参考 `tu_pipeline.cc:861` 以后显式处理 FS 不读取 clip/cull distances 时的 linkage，并发出对应 GRAS clip mask；与此前 viewport 变换公式一起解释了其处理边界。参考源码版本仍与实际安装驱动不同，不能当作精确驱动二进制的源码映射。

## 聚焦验证

设备 `9c2841a4`，Adreno 740 / API33 / 4 KiB：

| 检查 | 结果 |
|---|---|
| host 深度变换、启用条件、反向/常量范围 | 14,588 checks / 0 failures |
| Qualcomm 实际光栅化、FragCoord 和 D32 readback | 1,792 checks / 0 failures |
| Turnip 实际光栅化、FragCoord 和 D32 readback | 1,792 checks / 0 failures |
| 14 VS + FS + 三种辅助 tessellation SPIR-V | 18/18 `spirv-val --target-env vulkan1.2` |
| host RelWithDebInfo 与普通 APK 构建 | 通过 |

GPU probe 直接调用生产 `EmitPrologue`/`EmitEpilogue`，覆盖 identity、half range、reversed、负深度钳制、near/far refusal、单独关闭 near/far、custom clamp、constant range、部分三角形裁剪、非单位 W。它不是仅复刻数学的 GLSL 测试。GS、多 viewport 和辅助 tessellation 的全部 GPU 组合仍未被这个探针覆盖；辅助模块有独立 SPIR-V 校验。

早期 probe 有缺 capability、误用 `begin({})` 选择空指针重载的问题，已修正；初始失败及 separate-builtin 负例日志保留，不能混算成最终通过。

重跑命令：

```sh
c++ -std=c++20 -I src tests/video_core/depth_range_tests.cpp -o build/depth_range_tests
build/depth_range_tests
cmake --build build/android-host-api33/native --target android_depth_range_probe --parallel 6
# 将 probe 与匹配 host DSO 放入设备独立目录后：
LD_LIBRARY_PATH=. ./android_depth_range_probe system /data/local/tmp/shad-int64-probe
LD_LIBRARY_PATH=. ./android_depth_range_probe /data/local/tmp/shad-int64-probe /data/local/tmp/shad-int64-probe
```

## APK 与现场边界

最终 APK SHA256 `1176585cbe5b6a94757e6ca18d90d48773aef7adaa76d1fc622121b4df2cc825`；host `096afb0e96dcd429461a16e990cfe943202292c4cc7d876442d9dbef50e56b65`。APK 内 host 与本地构建完全一致。host/JNI 为 RelWithDebInfo，Kotlin 包变体为 playstoreDebug，FEX 仍 Release。

中间版系统驱动已到屋顶 MOVE，硬边雾、墙面和花盆的块状遮挡明显改善；它尚未包含最终的 ClipDistance block 兼容，因此不作为最终版裁剪验收。最终版 PID11724/gen1/UUID `c9027afc8510cc25216a9e8b4dcce73a` 在普通系统驱动、无 RenderDoc/调试器环境进入屋顶；frame10→12 的实际 stick-left 输入使人物和镜头移动，雾的过渡保持连续，原大片三角形硬边未再出现。最终 warmup `GAMEPLAY_REVIEWED` / exit0，观察描述以截图为准：frame12 仍为 MOVE，初次 review 文字误写 ATTACK，已更正并在 manifest 标注。约12 FPS，不作性能提升或全游戏画面无误声明。

![修复前](../../../build/viewport-depth-fix-20260917/before.png)
![最终系统驱动修复后](../../../build/viewport-depth-fix-20260917/after.png)

上图是两次普通 APK 运行、同一初始屋顶视角，动画时刻不同；不是同帧 replay 或逐像素 A/B。

原始包在本轮 debugger attach 开始前已经有一次 Present→`vkBeginCommandBuffer` 崩溃。中间版另有启动时 Guest-1 JIT SIGSEGV、返回 UI 时 HWUI/SkCanvas SIGSEGV；两者均存档，本轮没有把这些稳定性问题归因于或宣称由雾修复解决。没有完成十分钟稳定性、全场景、全游戏或性能回归。

实现限制：fallback 需要独立 depth clip 控制和 shaderClipDistance，必须有空闲 clip slots；与 CullDistance 或既有软件 clip emulation 同时使用、或 guest clamp bounds 超出 `[0,1]` 的组合会明确拒绝，尚未完整支持。支持 unrestricted range 的设备继续原路径。

全部中间日志、SPIR-V、截图、APK/源码 hash 和失败证据在 `build/viewport-depth-fix-20260917/`。保留之前的 dirty work；本轮未 commit/push，无新 spec。
