// SPDX-License-Identifier: GPL-2.0-or-later
// Real rasterization through production software-interpolation GS and FS emitters.
#include <cmath>
#include <cstdio>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_interpolation.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "video_core/renderer_vulkan/vk_instance.h"
using namespace Shader;
using namespace Shader::Backend::SPIRV;
struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 8;
    }
    s32 GetHeight() const override {
        return 8;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};

struct Case {
    const char* name;
    bool perspective, depth;
    u32 mode{};
    u32 raw_word{};
};
Info VertexInfo() {
    Info info{};
    info.hw_stage = HwStage::Vertex;
    info.sw_stage = SwStage::Vertex;
    info.stores.Set(IR::Attribute::Param0 + 5, 0);
    info.stores.Set(IR::Attribute::Param0 + 7, 0);
    return info;
}
RuntimeInfo VertexRuntime(const Case& test) {
    RuntimeInfo r{};
    r.hw_stage = HwStage::Vertex;
    r.sw_stage = SwStage::Vertex;
    r.depth_range.enabled = test.depth;
    r.depth_range.clip_near = test.depth;
    return r;
}
Info FragmentInfo() {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    info.loads.Set(IR::Attribute::Param0, 0);
    info.loads.Set(IR::Attribute::Param0 + 1, 0);
    info.loads.Set(IR::Attribute::BaryCoordSmooth, 0);
    info.loads.Set(IR::Attribute::BaryCoordSmooth, 1);
    info.loads.Set(IR::Attribute::BaryCoordNoPersp, 0);
    info.loads.Set(IR::Attribute::BaryCoordSmoothCentroid, 0);
    info.loads.Set(IR::Attribute::BaryCoordSmoothSample, 0);
    info.loads.Set(IR::Attribute::BaryCoordNoPerspCentroid, 0);
    info.loads.Set(IR::Attribute::BaryCoordNoPerspSample, 0);
    info.loads.Set(IR::Attribute::BaryCoordPullModel, 0);
    info.loads.Set(IR::Attribute::FragCoord, 3);
    info.fs_interpolation[0].primary = Qualifier::PerVertex;
    info.fs_interpolation[1].primary = Qualifier::Smooth;
    return info;
}
RuntimeInfo FragmentRuntime() {
    RuntimeInfo r{};
    r.hw_stage = HwStage::Fragment;
    r.sw_stage = SwStage::Fragment;
    r.hw.fs.num_inputs = 3;
    r.hw.fs.inputs[0] = {.param_index = 5, .is_default = true, .is_flat = true};
    r.hw.fs.inputs[1] = {.param_index = 7};
    r.hw.fs.inputs[2] = {.is_default = true};
    return r;
}
std::vector<u32> Vertex(const Case& test) {
    Sirit::Module c(0x10500);
    c.AddCapability(spv::Capability::Shader);
    if (test.depth)
        c.AddCapability(spv::Capability::ClipDistance);
    c.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    auto f = c.TypeFloat(32), u = c.TypeUInt(32), b = c.TypeBool(), v4 = c.TypeVector(f, 4),
         void_t = c.TypeVoid();
    const auto N = [&](u32 n) { return c.Constant(u, n); };
    const auto F = [&](float v) { return c.Constant(f, v); };
    const auto var = [&](Sirit::Id t, spv::StorageClass s) {
        return c.AddGlobalVariable(c.TypePointer(s, t), s);
    };
    auto id = var(u, spv::StorageClass::Input);
    c.Decorate(id, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
    Sirit::Id position;
    if (test.depth) {
        auto block = c.TypeStruct(v4, c.TypeArray(f, N(8)));
        c.Decorate(block, spv::Decoration::Block);
        c.MemberDecorate(block, 0, spv::Decoration::BuiltIn, u32(spv::BuiltIn::Position));
        c.MemberDecorate(block, 1, spv::Decoration::BuiltIn, u32(spv::BuiltIn::ClipDistance));
        position = var(block, spv::StorageClass::Output);
    } else {
        position = var(v4, spv::StorageClass::Output);
        c.Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    }
    auto packed = var(v4, spv::StorageClass::Output), ordinary = var(v4, spv::StorageClass::Output);
    c.Decorate(packed, spv::Decoration::Location, 5U);
    c.Decorate(ordinary, spv::Decoration::Location, 7U);
    auto main = c.OpFunction(void_t, spv::FunctionControlMask::MaskNone, c.TypeFunction(void_t));
    c.AddLabel();
    auto idx = c.OpLoad(u, id), one = c.OpIEqual(b, idx, N(1)), two = c.OpIEqual(b, idx, N(2));
    auto w = test.perspective ? c.OpSelect(f, one, F(2), c.OpSelect(f, two, F(4), F(1))) : F(1);
    auto x = c.OpFMul(f, c.OpSelect(f, one, F(3), F(-1)), w),
         y = c.OpFMul(f, c.OpSelect(f, two, F(3), F(-1)), w);
    auto pos = c.OpCompositeConstruct(v4, x, y, c.OpFMul(f, F(.5), w), w);
    c.OpStore(test.depth
                  ? c.OpAccessChain(c.TypePointer(spv::StorageClass::Output, v4), position, N(0))
                  : position,
              pos);
    if (test.depth)
        for (u32 i = 0; i < 8; ++i)
            c.OpStore(
                c.OpAccessChain(c.TypePointer(spv::StorageClass::Output, f), position, N(1), N(i)),
                F(1));
    auto bits = c.OpSelect(u, one, N(0x3c003800), c.OpSelect(u, two, N(0x3c003b00), N(0x3c003000)));
    if (test.mode == 4)
        bits = c.OpSelect(u, one, N(test.raw_word ^ 0x00550055u),
                          c.OpSelect(u, two, N(test.raw_word ^ 0x002a002au), N(test.raw_word)));
    c.OpStore(packed, c.OpCompositeConstruct(v4, c.OpBitcast(f, bits), F(0), F(0), F(1)));
    c.OpStore(ordinary,
              c.OpCompositeConstruct(v4, c.OpSelect(f, one, F(1), F(0)), F(0), F(0), F(1)));
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main",
                    std::array{id, position, packed, ordinary});
    return c.Assemble();
}
// khr: the same fragment emitter on the driver's VK_KHR_fragment_shader_barycentric, no GS.
std::vector<u32> Fragment(bool negative, u32 mode, bool khr) {
    Profile p{};
    p.supported_spirv = 0x10500;
    p.emulate_fragment_interpolation = !khr;
    p.supports_fragment_shader_barycentric = khr;
    auto info = FragmentInfo();
    auto r = FragmentRuntime();
    Shader::Backend::Bindings bindings{};
    EmitContext c(p, r, info, bindings);
    if (khr) {
        c.AddExtension("SPV_KHR_fragment_shader_barycentric");
        c.AddCapability(spv::Capability::FragmentBarycentricKHR);
    }
    c.AddCapability(spv::Capability::Int8);
    c.AddCapability(spv::Capability::Int16);
    c.AddCapability(spv::Capability::InterpolationFunction);
    c.AddCapability(spv::Capability::SampleRateShading);
    auto color = c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Output, c.F32[4]),
                                     spv::StorageClass::Output);
    c.Decorate(color, spv::Decoration::Location, 0U);
    c.interfaces.push_back(color);
    auto main =
        c.OpFunction(c.void_id, spv::FunctionControlMask::MaskNone, c.TypeFunction(c.void_id));
    c.AddLabel();
    const auto smooth = mode == 1   ? IR::Attribute::BaryCoordSmoothCentroid
                        : mode == 2 ? IR::Attribute::BaryCoordSmoothSample
                                    : IR::Attribute::BaryCoordSmooth;
    auto i = EmitGetAttribute(c, smooth, 0, 0), j = EmitGetAttribute(c, smooth, 1, 0);
    if (mode == 3) {
        const auto iw = EmitGetAttribute(c, IR::Attribute::BaryCoordPullModel, 2, 0);
        i = c.OpFDiv(c.F32[1], EmitGetAttribute(c, IR::Attribute::BaryCoordPullModel, 0, 0), iw);
        j = c.OpFDiv(c.F32[1], EmitGetAttribute(c, IR::Attribute::BaryCoordPullModel, 1, 0), iw);
    }
    std::array<Sirit::Id, 3> weights{c.OpFSub(c.F32[1], c.ConstF32(1.f), c.OpFAdd(c.F32[1], i, j)),
                                     i, j};
    auto sum = c.ConstF32(0.f);
    for (u32 n = 0; n < 3; ++n) {
        auto a = EmitGetAttribute(c, IR::Attribute::Param0, 0, negative ? 0 : n);
        auto decoded = c.OpUnpackHalf2x16(c.F32[2], c.OpBitcast(c.U32[1], a));
        sum = c.OpFAdd(c.F32[1], sum,
                       c.OpFMul(c.F32[1], weights[n], c.OpCompositeExtract(c.F32[1], decoded, 0)));
    }
    const auto linear_attr = mode == 1   ? IR::Attribute::BaryCoordNoPerspCentroid
                             : mode == 2 ? IR::Attribute::BaryCoordNoPerspSample
                                         : IR::Attribute::BaryCoordNoPersp;
    auto linear = EmitGetAttribute(c, linear_attr, 0, 0);
    auto regular = EmitGetAttribute(c, IR::Attribute::Param0 + 1, 0, 0);
    if (mode == 4) {
        // Compare integers in the fragment and export small finite floats, so an
        // attachment's NaN conversion cannot conceal transport corruption.
        std::array<Sirit::Id, 3> words;
        for (u32 n = 0; n < 3; ++n)
            words[n] = c.OpBitcast(c.U32[1], EmitGetAttribute(c, IR::Attribute::Param0, 0, n));
        const auto a =
            c.OpConvertUToF(c.F32[1], c.OpBitwiseAnd(c.U32[1], words[0], c.ConstU32(0xffffu)));
        const auto b =
            c.OpConvertUToF(c.F32[1], c.OpShiftRightLogical(c.U32[1], words[0], c.ConstU32(16u)));
        const auto x = c.OpConvertUToF(c.F32[1], c.OpBitwiseXor(c.U32[1], words[0], words[1]));
        const auto y = c.OpConvertUToF(c.F32[1], c.OpBitwiseXor(c.U32[1], words[0], words[2]));
        c.OpStore(color, c.OpCompositeConstruct(c.F32[4], a, b, x, y));
    } else {
        c.OpStore(color, c.OpCompositeConstruct(c.F32[4], sum, linear, regular, c.ConstF32(1.f)));
    }
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    c.AddEntryPoint(spv::ExecutionModel::Fragment, main, "main", c.interfaces);
    return c.Assemble();
}
int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("depth-probe");
    Window window;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    auto d = instance.GetDevice();
    auto memory = [&](vk::MemoryRequirements req, vk::MemoryPropertyFlags flags) {
        auto mp = instance.GetMemoryProperties();
        u32 i = 0;
        while (i < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << i)) ||
                                          (mp.memoryTypes[i].propertyFlags & flags) != flags))
            ++i;
        if (i == mp.memoryTypeCount)
            throw std::runtime_error("no memory type");
        return Vulkan::Check(
            d.allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = i}));
    };
    auto image =
        Vulkan::Check(d.createImageUnique({.imageType = vk::ImageType::e2D,
                                           .format = vk::Format::eR32G32B32A32Sfloat,
                                           .extent = {8, 8, 1},
                                           .mipLevels = 1,
                                           .arrayLayers = 1,
                                           .samples = vk::SampleCountFlagBits::e1,
                                           .tiling = vk::ImageTiling::eOptimal,
                                           .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                    vk::ImageUsageFlagBits::eTransferSrc}));
    auto imem =
        memory(d.getImageMemoryRequirements(*image), vk::MemoryPropertyFlagBits::eDeviceLocal);
    Vulkan::Check(d.bindImageMemory(*image, *imem, 0));
    auto view = Vulkan::Check(d.createImageViewUnique(
        {.image = *image,
         .viewType = vk::ImageViewType::e2D,
         .format = vk::Format::eR32G32B32A32Sfloat,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
    auto depth_image = Vulkan::Check(
        d.createImageUnique({.imageType = vk::ImageType::e2D,
                             .format = vk::Format::eD32Sfloat,
                             .extent = {8, 8, 1},
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = vk::SampleCountFlagBits::e1,
                             .tiling = vk::ImageTiling::eOptimal,
                             .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                      vk::ImageUsageFlagBits::eTransferSrc}));
    auto depth_mem = memory(d.getImageMemoryRequirements(*depth_image),
                            vk::MemoryPropertyFlagBits::eDeviceLocal);
    Vulkan::Check(d.bindImageMemory(*depth_image, *depth_mem, 0));
    auto depth_view = Vulkan::Check(d.createImageViewUnique(
        {.image = *depth_image,
         .viewType = vk::ImageViewType::e2D,
         .format = vk::Format::eD32Sfloat,
         .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}}));
    auto buffer = Vulkan::Check(
        d.createBufferUnique({.size = 8 * 8 * 20, .usage = vk::BufferUsageFlagBits::eTransferDst}));
    auto mem = memory(d.getBufferMemoryRequirements(*buffer),
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent);
    Vulkan::Check(d.bindBufferMemory(*buffer, *mem, 0));
    auto mapped = static_cast<float*>(Vulkan::Check(d.mapMemory(*mem, 0, VK_WHOLE_SIZE)));
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(PushData)};
    auto layout = Vulkan::Check(
        d.createPipelineLayoutUnique({.pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
    auto module = [&](const std::vector<u32>& words, const std::string& name) {
        std::ofstream out(std::string(argv[2]) + "/" + name + ".spv", std::ios::binary);
        out.write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
        return Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = words.size() * 4, .pCode = words.data()}));
    };
    const bool negative = std::getenv("INTERPOLATION_NEGATIVE") != nullptr;
    const bool khr = std::getenv("INTERPOLATION_KHR") != nullptr;
    printf("mode=%s driver_barycentric=%d\n", khr ? "khr" : "software-gs",
           instance.IsFragmentShaderBarycentricSupported());
    if (khr && !instance.IsFragmentShaderBarycentricSupported())
        return 3;

    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto commands = Vulkan::Check(d.allocateCommandBuffers(
        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    const Case cases[] = {{"affine", false, false},
                          {"perspective", true, false},
                          {"clip-block", true, true},
                          {"centroid", true, false, 1},
                          {"sample", true, false, 2},
                          {"pull-model", true, false, 3},
                          {"raw-positive-nan", false, false, 4, 0x7fc12345u},
                          {"raw-negative-nan", false, false, 4, 0xffc54321u},
                          {"raw-signaling-nan", false, false, 4, 0x7f812345u},
                          {"raw-positive-infinity", false, false, 4, 0x7f800000u},
                          {"raw-negative-infinity", false, false, 4, 0xff800000u},
                          {"raw-subnormal", false, false, 4, 0x00001234u},
                          {"raw-negative-zero", false, false, 4, 0x80000000u},
                          {"raw-all-ones", false, false, 4, 0xffffffffu}};
    unsigned checks = 0, failures = 0;
    for (const auto& test : cases) {
        auto fs = module(Fragment(negative, test.mode, khr), std::string(test.name) + "-fragment");
        auto vs = module(Vertex(test), test.name);
        std::vector<vk::PipelineShaderStageCreateInfo> stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
        vk::UniqueShaderModule gs;
        if (!khr) {
            auto vi = VertexInfo();
            auto vr = VertexRuntime(test);
            auto fi = FragmentInfo();
            auto fr = FragmentRuntime();
            gs = module(EmitSoftwareInterpolationGeometry(vi, vr, fi, fr.hw.fs, 128, 1024),
                        std::string(test.name) + "-geometry");
            stages.push_back({.stage = vk::ShaderStageFlagBits::eGeometry,
                              .module = *gs,
                              .pName = "main"});
        }
        vk::PipelineVertexInputStateCreateInfo vertex{};
        vk::PipelineInputAssemblyStateCreateInfo ia{.topology =
                                                        vk::PrimitiveTopology::eTriangleList};

        vk::Viewport viewport{0, 0, 8, 8, 0, 1};
        vk::Rect2D scissor{{0, 0}, {8, 8}};
        vk::PipelineViewportStateCreateInfo vp{
            .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT clip{.depthClipEnable = false};
        vk::PipelineRasterizationStateCreateInfo raster{.pNext = &clip,
                                                        .depthClampEnable = true,
                                                        .polygonMode = vk::PolygonMode::eFill,
                                                        .lineWidth = 1};
        vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                      vk::SampleCountFlagBits::e1};
        vk::PipelineColorBlendAttachmentState blend{
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &blend};
        vk::Format format = vk::Format::eR32G32B32A32Sfloat;
        vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                  .pColorAttachmentFormats = &format,
                                                  .depthAttachmentFormat = vk::Format::eD32Sfloat};
        vk::PipelineDepthStencilStateCreateInfo ds{.depthTestEnable = true,
                                                   .depthWriteEnable = true,
                                                   .depthCompareOp = vk::CompareOp::eAlways};
        auto pipeline =
            Vulkan::Check(d.createGraphicsPipelineUnique({}, {.pNext = &rendering,
                                                              .stageCount = u32(stages.size()),
                                                              .pStages = stages.data(),
                                                              .pVertexInputState = &vertex,
                                                              .pInputAssemblyState = &ia,
                                                              .pViewportState = &vp,
                                                              .pRasterizationState = &raster,
                                                              .pMultisampleState = &ms,
                                                              .pDepthStencilState = &ds,
                                                              .pColorBlendState = &cb,
                                                              .layout = *layout}));
        auto cmd = commands[0];
        Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
        vk::ImageMemoryBarrier barrier{
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, barrier);
        auto depth_barrier = barrier;
        depth_barrier.image = *depth_image;
        depth_barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
        depth_barrier.dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        depth_barrier.newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eEarlyFragmentTests, {}, {}, {},
                            depth_barrier);
        vk::RenderingAttachmentInfo depth_attachment{
            .imageView = *depth_view,
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = {.depthStencil = {.depth = .123f}}};
        vk::RenderingAttachmentInfo attachment{
            .imageView = *view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = {.color =
                               vk::ClearColorValue{.float32 = std::array{-5.f, -5.f, -5.f, -5.f}}}};
        cmd.beginRendering({.renderArea = scissor,
                            .layerCount = 1,
                            .colorAttachmentCount = 1,
                            .pColorAttachments = &attachment,
                            .pDepthAttachment = &depth_attachment});
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();
        barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
        cmd.copyImageToBuffer(
            *image, vk::ImageLayout::eTransferSrcOptimal, *buffer,
            vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                .imageExtent = {8, 8, 1}});
        depth_barrier.srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        depth_barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        depth_barrier.oldLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        depth_barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits::eLateFragmentTests,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, depth_barrier);
        cmd.copyImageToBuffer(
            *depth_image, vk::ImageLayout::eTransferSrcOptimal, *buffer,
            vk::BufferImageCopy{.bufferOffset = 8 * 8 * 16,
                                .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
                                .imageExtent = {8, 8, 1}});
        Vulkan::Check(cmd.end());
        Vulkan::Check(instance.GetGraphicsQueue().submit(
            vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, *fence));
        if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
            return 4;
        const unsigned before = failures;
        for (unsigned i = 0; i < 64; i++) {
            const float l1 = (float(i % 8) + .5f) / 16, l2 = (float(i / 8) + .5f) / 16,
                        l0 = 1 - l1 - l2;
            const float b1 = l1 / (test.perspective ? 2 : 1), b2 = l2 / (test.perspective ? 4 : 1),
                        den = l0 + b1 + b2;
            std::array<float, 4> expected{(.125f * l0 + .5f * b1 + .875f * b2) / den, l1, b1 / den,
                                          1};
            if (test.mode == 4)
                expected = {float(test.raw_word & 0xffffu), float(test.raw_word >> 16),
                            float(0x00550055u), float(0x002a002au)};
            for (u32 c = 0; c < 4; ++c) {
                ++checks;
                if (!std::isfinite(mapped[i * 4 + c]) ||
                    std::abs(mapped[i * 4 + c] - expected[c]) > 2e-5f) {
                    if (failures < 5)
                        printf("pixel=%u comp=%u got=%f expected=%f\n", i, c, mapped[i * 4 + c],
                               expected[c]);
                    ++failures;
                }
            }
            ++checks;
            if (std::abs(mapped[256 + i] - .5f) > 2e-6f)
                ++failures;
        }
        printf("%s failures=%u\n", test.name, failures - before);
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    }
    d.unmapMemory(*mem);
    printf("INTERPOLATION_DEVICE %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    const int result = Run(argc, argv);
    fflush(nullptr);
    _Exit(result);
}
