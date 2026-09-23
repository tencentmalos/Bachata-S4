# Vulkan：辅助图元阶段的 Layer 与 ViewportIndex

2026-09-21，公开 Khronos 主来源；不是 Sony SDK 文档。

- [Layer](https://docs.vulkan.org/refpages/latest/refpages/source/Layer.html)：决定 framebuffer attachment 的数组层；以最终活动的前光栅化阶段输出为准。未由该阶段输出时，使用第0层。一个图元各顶点必须使用一致的层值。
- [Shader interfaces](https://docs.vulkan.org/spec/latest/chapters/interfaces.html)：跨 shader stage 的接口按 storage、location/type 与 builtin 合同匹配。不能仅在VS输出Layer后插入未传递它的TCS/TES，并期望原值继续决定光栅化。
- [Tessellation](https://docs.vulkan.org/spec/latest/chapters/tessellation.html)：TCS控制点数据与TES输出是独立阶段；图元离散索引应明确传递，不能当作普通浮点颜色/坐标插值。

本仓处理：rect/quad模拟用空闲user location桥接整数Layer/ViewportIndex，TCS保留每图元值，最终TES再导出builtin。原生无辅助阶段的VS路径保持直接builtin输出。GPU像素验证与反例见 [验证记录](../../docs/validation/android-native-host/sports-layered-primitive-20260921.md)。
