# Vulkan MSAA / viewport 排查参考

2026-09-21，公开 Khronos 文档；这是参考笔记，不是 Sony SDK 文档。

- [VkImageCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageCreateInfo.html)：VUID `samples-02258` 要求实际 samples 属于该图像创建条件允许的采样集合；`usage-00968` 规定未启用 shaderStorageImageMultisample 时，STORAGE 图像只能使用单采样。额外预留用途会约束合法配置，不能只查询 framebuffer 的全局 sample mask。
- [Dynamic Rendering 设计说明](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering.html)：混合附件采样需要对应扩展和采样数结构；省略该结构时，各附件采样数按 pipeline 的 rasterizationSamples 解释。
- [Multisampled render to single sampled](https://docs.vulkan.org/refpages/latest/refpages/source/VkMultisampledRenderToSingleSampledInfoEXT.html)：这条路径需要显式功能和 rendering pNext 配置，不能用来解释没有这些配置的1×颜色/2×pipeline组合。

本机独立查询及游戏实景见 [Beat Saber MSAA 修复报告](../../docs/validation/android-native-host/beatsaber-viewport-msaa-20260921.md)。
实际驱动结果、原始捕获命令和当前配置优先于泛化推测；正确 viewport 数值不能替代附件兼容性检查。

续项（2026-09-21）：[SPIR-V 规范](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html) 的 image Sample 操作数与 MS 类型必须匹配。强制单采样需要同时改物理 image、pipeline 及生成的 shader 类型/操作数；不能只改 rasterizationSamples。[Vulkan render pass](https://docs.vulkan.org/spec/latest/chapters/renderpass.html) 和 [rasterization](https://docs.vulkan.org/spec/latest/chapters/primsrast.html) 的采样合同仍适用。实际双驱动测试与开关语义见 [MSAA 策略报告](../../docs/validation/android-native-host/msaa-policy-gyro-20260921.md)。
