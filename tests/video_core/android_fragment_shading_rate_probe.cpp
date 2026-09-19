// SPDX-License-Identifier: GPL-2.0-or-later
// GPU readback checks actual fragment grouping and guest/host/guest transitions.
#include <cstdio>
#include <map>
#include <set>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "video_core/renderer_vulkan/vk_instance.h"
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
std::vector<u32> ShaderCode(bool fragment) {
    Sirit::Module c(0x10500);
    c.AddCapability(spv::Capability::Shader);
    c.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    auto f = c.TypeFloat(32), v = c.TypeVector(f, 4), u = c.TypeInt(32, false);
    auto void_t = c.TypeVoid();
    auto input = c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Input, fragment ? v : u),
                                     spv::StorageClass::Input);
    auto output =
        c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Output, v), spv::StorageClass::Output);
    c.Decorate(input, spv::Decoration::BuiltIn,
               fragment ? spv::BuiltIn::FragCoord : spv::BuiltIn::VertexIndex);
    if (fragment)
        c.Decorate(output, spv::Decoration::Location, 0U);
    else
        c.Decorate(output, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    auto main = c.OpFunction(void_t, spv::FunctionControlMask::MaskNone, c.TypeFunction(void_t));
    c.AddLabel();
    if (fragment) {
        c.OpStore(output, c.OpLoad(v, input));
    } else {
        auto idx = c.OpLoad(u, input);
        auto x = c.OpSelect(f, c.OpIEqual(c.TypeBool(), idx, c.Constant(u, 1U)), c.Constant(f, 3.f),
                            c.Constant(f, -1.f));
        auto y = c.OpSelect(f, c.OpIEqual(c.TypeBool(), idx, c.Constant(u, 2U)), c.Constant(f, 3.f),
                            c.Constant(f, -1.f));
        c.OpStore(output, c.OpCompositeConstruct(v, x, y, c.Constant(f, 0.f), c.Constant(f, 1.f)));
    }
    c.OpReturn();
    c.OpFunctionEnd();
    if (fragment)
        c.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    c.AddEntryPoint(fragment ? spv::ExecutionModel::Fragment : spv::ExecutionModel::Vertex, main,
                    "main", std::array{input, output});
    return c.Assemble();
}
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("shading-rate-probe");
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
    if (instance.IsFdmSupported())
        return 6;
    if (!instance.IsPipelineFragmentShadingRateSupported()) {
        printf("SHADING_RATE_UNSUPPORTED: full rate retained; GPU coarse tests skipped\n");
        return 77;
    }
    constexpr u32 W = 96, H = 32;
    auto image =
        Vulkan::Check(d.createImageUnique({.imageType = vk::ImageType::e2D,
                                           .format = vk::Format::eR32G32B32A32Sfloat,
                                           .extent = {W, H, 1},
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
    auto buffer = Vulkan::Check(
        d.createBufferUnique({.size = W * H * 16, .usage = vk::BufferUsageFlagBits::eTransferDst}));
    auto mem = memory(d.getBufferMemoryRequirements(*buffer),
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent);
    Vulkan::Check(d.bindBufferMemory(*buffer, *mem, 0));
    auto mapped = static_cast<float*>(Vulkan::Check(d.mapMemory(*mem, 0, VK_WHOLE_SIZE)));
    auto indirect = Vulkan::Check(
        d.createBufferUnique({.size = 16, .usage = vk::BufferUsageFlagBits::eIndirectBuffer}));
    auto indirect_mem = memory(d.getBufferMemoryRequirements(*indirect),
                               vk::MemoryPropertyFlagBits::eHostVisible |
                                   vk::MemoryPropertyFlagBits::eHostCoherent);
    Vulkan::Check(d.bindBufferMemory(*indirect, *indirect_mem, 0));
    auto args = Vulkan::Check(d.mapMemory(*indirect_mem, 0, VK_WHOLE_SIZE));
    const vk::DrawIndirectCommand draw{3, 1, 0, 0};
    std::memcpy(args, &draw, sizeof(draw));
    d.unmapMemory(*indirect_mem);
    auto layout = Vulkan::Check(d.createPipelineLayoutUnique({}));
    auto module = [&](bool fragment) {
        const auto words = ShaderCode(fragment);
        return Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = words.size() * 4, .pCode = words.data()}));
    };
    auto vs = module(false), fs = module(true);
    std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
    auto make_pipeline = [&](bool guest, bool per_sample) {
        vk::PipelineVertexInputStateCreateInfo vertex{};
        vk::PipelineInputAssemblyStateCreateInfo ia{.topology =
                                                        vk::PrimitiveTopology::eTriangleList};
        vk::Viewport viewport{0, 0, W, H, 0, 1};
        vk::PipelineViewportStateCreateInfo vp{
            .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1};
        vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill,
                                                        .lineWidth = 1};
        vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                      vk::SampleCountFlagBits::e1,
                                                  .sampleShadingEnable = per_sample,
                                                  .minSampleShading = 1.f};
        vk::PipelineColorBlendAttachmentState blend{
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &blend};
        vk::Format format = vk::Format::eR32G32B32A32Sfloat;
        vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                  .pColorAttachmentFormats = &format};
        const std::array states{vk::DynamicState::eScissor,
                                vk::DynamicState::eFragmentShadingRateKHR};
        vk::PipelineDynamicStateCreateInfo dyn{.dynamicStateCount = guest ? 2U : 1U,
                                               .pDynamicStates = states.data()};
        return Vulkan::Check(d.createGraphicsPipelineUnique({}, {.pNext = &rendering,
                                                                 .stageCount = 2,
                                                                 .pStages = stages.data(),
                                                                 .pVertexInputState = &vertex,
                                                                 .pInputAssemblyState = &ia,
                                                                 .pViewportState = &vp,
                                                                 .pRasterizationState = &raster,
                                                                 .pMultisampleState = &ms,
                                                                 .pColorBlendState = &cb,
                                                                 .pDynamicState = &dyn,
                                                                 .layout = *layout}));
    };
    auto guest = make_pipeline(true, false), host = make_pipeline(false, false),
         sample = make_pipeline(true, true);
    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto commands = Vulkan::Check(d.allocateCommandBuffers(
        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    unsigned checks = 0, failures = 0;
    const auto& rates = instance.GetFragmentShadingRates();
    // A capability table which only supports 1x samples must not allow 8x MSAA.
    const Vulkan::FragmentShadingRates single{vk::SampleCountFlagBits::e1,
                                              vk::SampleCountFlagBits::e1};
    for (u32 q : {0U, 1U, 2U, 99U}) {
        ++checks;
        if (single.Select(q, vk::SampleCountFlagBits::e8) != vk::Extent2D{1, 1})
            ++failures;
    }
    for (u32 q : {2U, 1U, 0U, 2U, 0U, 1U, 99U}) {
        for (bool full : {false, true}) {
            const std::array sizes{
                rates.Select(q, vk::SampleCountFlagBits::e1, full), vk::Extent2D{1, 1},
                rates.Select(q == 0 ? 1U : 0U, vk::SampleCountFlagBits::e1, full)};
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
                                vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                                barrier);
            vk::RenderingAttachmentInfo attachment{.imageView = *view,
                                                   .imageLayout =
                                                       vk::ImageLayout::eColorAttachmentOptimal,
                                                   .loadOp = vk::AttachmentLoadOp::eClear,
                                                   .storeOp = vk::AttachmentStoreOp::eStore};
            cmd.beginRendering({.renderArea = {{0, 0}, {W, H}},
                                .layerCount = 1,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &attachment});
            for (u32 panel = 0; panel < 3; ++panel) {
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                 panel == 1 ? *host : (full ? *sample : *guest));
                cmd.setScissor(0, vk::Rect2D{{s32(panel * 32), 0}, {32, 32}});
                if (panel != 1)
                    Vulkan::SetGuestFragmentShadingRate(cmd, sizes[panel]);
                if (panel == 2)
                    cmd.drawIndirect(*indirect, 0, 1, sizeof(draw));
                else
                    cmd.draw(3, 1, 0, 0);
            }
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
                                    .imageExtent = {W, H, 1}});
            const vk::MemoryBarrier host_read{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                              .dstAccessMask = vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, host_read, {}, {});
            Vulkan::Check(cmd.end());
            Vulkan::Check(instance.GetGraphicsQueue().submit(
                vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, *fence));
            if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
                return 4;
            for (u32 panel = 0; panel < 3; ++panel) {
                std::map<std::pair<float, float>, unsigned> groups;
                for (u32 y = 0; y < H; ++y)
                    for (u32 x = panel * 32; x < (panel + 1) * 32; ++x) {
                        const auto* color = mapped + 4 * (y * W + x);
                        ++groups[{color[0], color[1]}];
                    }
                const u32 area = sizes[panel].width * sizes[panel].height;
                ++checks;
                if (groups.size() != 1024 / area)
                    ++failures;
                for (auto [coord, count] : groups) {
                    ++checks;
                    if (count != area)
                        ++failures;
                }
                printf("quality=%u full=%u panel=%u rate=%ux%u unique=%zu expected=%u\n", q, full,
                       panel, sizes[panel].width, sizes[panel].height, groups.size(), 1024 / area);
            }
            Vulkan::Check(d.resetFences(*fence));
            Vulkan::Check(d.resetCommandPool(*cp));
        }
    }
    d.unmapMemory(*mem);
    printf("SHADING_RATE_DEVICE %u checks / %u failures FDM=off\n", checks, failures);
    return failures ? 1 : 0;
}
