// SPDX-License-Identifier: GPL-2.0-or-later
// Real rasterization using the production position epilogue, not a GLSL copy.
#include <cmath>
#include <cstdio>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
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
    float scale, offset, z, w, lo, hi;
    bool negative, near, far;
};
std::vector<u32> Vertex(const Case& test) {
    Profile p{};
    p.supported_spirv = 0x10500;
    Info info{};
    info.stage = Stage::Vertex;
    info.l_stage = LogicalStage::Vertex;
    RuntimeInfo r{};
    r.stage = Stage::Vertex;
    r.depth_range.enabled = true;
    r.depth_range.negative_one_to_one = test.negative;
    r.depth_range.clip_near = test.near;
    r.depth_range.clip_far = test.far;
    r.depth_range.viewports[0] =
        AmdGpu::MakeDepthRangeTransform(test.scale, test.offset, test.lo, test.hi);
    Shader::Backend::Bindings bindings{};
    EmitContext c(p, r, info, bindings);
    const auto f = [&](float v) { return c.Constant(c.F32[1], v); };
    c.AddCapability(spv::Capability::DrawParameters);
    c.AddCapability(spv::Capability::Int8);
    c.AddCapability(spv::Capability::Int16);
    auto vid = c.vertex_index;
    auto main =
        c.OpFunction(c.void_id, spv::FunctionControlMask::MaskNone, c.TypeFunction(c.void_id));
    c.AddLabel();
    EmitPrologue(c);
    auto idx = c.OpLoad(c.U32[1], vid);
    auto x =
        c.OpSelect(c.F32[1], c.OpIEqual(c.U1[1], idx, c.ConstU32(1U)), f(3 * test.w), f(-test.w));
    auto y =
        c.OpSelect(c.F32[1], c.OpIEqual(c.U1[1], idx, c.ConstU32(2U)), f(3 * test.w), f(-test.w));
    auto z = f(test.z);
    if (std::string_view(test.name).starts_with("partial-"))
        z = c.OpSelect(c.F32[1], c.OpIEqual(c.U1[1], idx, c.ConstU32(1U)), f(test.z + 2.f), z);
    c.OpStore(c.output_position, c.OpCompositeConstruct(c.F32[4], x, y, z, f(test.w)));
    EmitEpilogue(c);
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", c.interfaces);
    return c.Assemble();
}
std::vector<u32> Fragment() {
    Sirit::Module c(0x10500);
    c.AddCapability(spv::Capability::Shader);
    c.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    auto f = c.TypeFloat(32), v = c.TypeVector(f, 4), void_t = c.TypeVoid();
    auto coord =
        c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Input, v), spv::StorageClass::Input);
    auto color =
        c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Output, v), spv::StorageClass::Output);
    c.Decorate(coord, spv::Decoration::BuiltIn, spv::BuiltIn::FragCoord);
    c.Decorate(color, spv::Decoration::Location, 0U);
    auto main = c.OpFunction(void_t, spv::FunctionControlMask::MaskNone, c.TypeFunction(void_t));
    c.AddLabel();
    auto z = c.OpCompositeExtract(f, c.OpLoad(v, coord), 2U);
    c.OpStore(color, c.OpCompositeConstruct(v, z, z, z, c.Constant(f, 1.f)));
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    c.AddEntryPoint(spv::ExecutionModel::Fragment, main, "main", std::array{coord, color});
    return c.Assemble();
}
int main(int argc, char** argv) {
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
    auto fs = module(Fragment(), "fragment");
    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto commands = Vulkan::Check(d.allocateCommandBuffers(
        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    const Case cases[] = {
        {"identity", 1, 0, .9584788f, 1, 0, 1, true, true, true},
        {"half", .5, .5, .9584788f, 1, 0, 1, true, true, true},
        {"reversed", -1, 1, .75, 1, 0, 1, true, true, true},
        {"negative-clamp", 1, 0, -.5, 1, 0, 1, true, true, true},
        {"near-reject", 1, 0, -1.1, 1, 0, 1, true, true, true},
        {"far-reject", 1, 0, 1.1, 1, 0, 1, true, true, true},
        {"zero-near-reject", 1, 0, -.1, 1, 0, 1, false, true, true},
        {"near-disabled", 1, 0, -2, 1, 0, 1, true, false, true},
        {"far-disabled", 1, 0, 2, 1, 0, 1, true, true, false},
        {"custom-clamp", 2, -.25, .4, 1, .2, .8, true, true, true},
        {"constant", 1, 0, .6, 1, .3, .3, true, true, true},
        {"partial-near", 1, 0, -1.5f, 1, 0, 1, true, true, true},
        {"partial-far", 1, 0, .5f, 1, 0, 1, true, true, true},
        {"perspective", 1, 0, 32.67376f, 34.12475f, 0, 1, true, true, true},
    };
    unsigned checks = 0, failures = 0;
    for (const auto& test : cases) {
        auto vs = module(Vertex(test), test.name);
        std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
        vk::PipelineVertexInputStateCreateInfo vertex{};
        vk::PipelineInputAssemblyStateCreateInfo ia{.topology =
                                                        vk::PrimitiveTopology::eTriangleList};
        auto t = AmdGpu::MakeDepthRangeTransform(test.scale, test.offset, test.lo, test.hi);
        vk::Viewport viewport{0, 0, 8, 8, t.min_depth, t.max_depth};
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
                                                              .stageCount = 2,
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
            float z = test.z;
            if (std::string_view(test.name).starts_with("partial-"))
                z += (float(i % 8) + .5f) / 8.f;
            const bool reject =
                (test.near && z < (test.negative ? -test.w : 0.f)) || (test.far && z > test.w);
            const float coord =
                test.lo == test.hi ? test.lo : test.scale * z / test.w + test.offset;
            const float expected_coord = reject ? -5.f : coord;
            const float expected_depth =
                reject ? .123f
                       : std::clamp(coord, std::min(test.lo, test.hi), std::max(test.lo, test.hi));
            checks += 2;
            if (std::abs(mapped[i * 4] - expected_coord) > 2e-6f)
                ++failures;
            if (std::abs(mapped[256 + i] - expected_depth) > 2e-6f)
                ++failures;
        }
        printf("%s coord=%.8f depth=%.8f failures=%u\n", test.name, mapped[4 * (4 * 8 + 4)],
               mapped[256 + 4 * 8 + 4], failures - before);
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    }
    const std::array<u32, 0> locations{};
    for (auto type :
         {AuxShaderType::RectListTCS, AuxShaderType::QuadListTCS, AuxShaderType::PassthroughTES})
        module(EmitAuxilaryTessShader(type, locations, true), "aux-" + std::to_string(u32(type)));
    d.unmapMemory(*mem);
    printf("DEPTH_DEVICE %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
