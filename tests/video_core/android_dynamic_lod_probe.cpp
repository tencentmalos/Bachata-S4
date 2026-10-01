// SPDX-License-Identifier: GPL-2.0-or-later
// Fragment quad divergence through the production GuardedResource SPIR-V emitter.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "video_core/renderer_vulkan/vk_instance.h"
using namespace Shader;
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
    AmdGpu::ImageType type;
    float bias;
    bool depth{};
    float sampler_bias{};
    float base_lod{2.f};
    bool limit_diagnostic{};
};
std::vector<u32> Vertex() {
    Sirit::Module c(0x10500);
    c.AddCapability(spv::Capability::Shader);
    c.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    auto f = c.TypeFloat(32), u = c.TypeUInt(32), b = c.TypeBool(), v4 = c.TypeVector(f, 4),
         v = c.TypeVoid();
    auto N = [&](u32 n) { return c.Constant(u, n); };
    auto F = [&](float n) { return c.Constant(f, n); };
    auto index =
        c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Input, u), spv::StorageClass::Input);
    auto position = c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Output, v4),
                                        spv::StorageClass::Output);
    c.Decorate(index, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
    c.Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    auto main = c.OpFunction(v, spv::FunctionControlMask::MaskNone, c.TypeFunction(v));
    c.AddLabel();
    auto id = c.OpLoad(u, index);
    c.OpStore(position, c.OpCompositeConstruct(
                            v4, c.OpSelect(f, c.OpIEqual(b, id, N(1)), F(3), F(-1)),
                            c.OpSelect(f, c.OpIEqual(b, id, N(2)), F(3), F(-1)), F(.5), F(1)));
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", std::array{index, position});
    return c.Assemble();
}
std::vector<u32> Fragment(const Case& test, bool guarded) {
    Common::ObjectPool<IR::Inst> pool;
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    AmdGpu::Image sharp = AmdGpu::Image::Null(test.depth);
    sharp.type = u64(test.type);
    sharp.width = 31;
    sharp.height = 31;
    sharp.depth = 5;
    sharp.last_level = 5;
    for (u32 i = 0; i < 2; ++i) {
        ImageResource r{};
        r.is_depth = test.depth;
        r.is_array = true;
        std::memcpy(r.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
        info.images.push_back(r);
    }
    SamplerResource sr{.is_depth = test.depth};
    AmdGpu::Sampler ss{};
    ss.lod_bias.Assign(static_cast<u64>(static_cast<s32>(test.sampler_bias * 256)) & 0x3fff);
    std::memcpy(sr.sharp_fetch.immediates.data(), &ss, sizeof(ss));
    info.samplers.push_back(sr);
    IR::Block block(pool);
    IR::Program program(info);
    IR::IREmitter ir(block);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    auto x = ir.GetAttribute(IR::Attribute::FragCoord, 0),
         y = ir.GetAttribute(IR::Attribute::FragCoord, 1);
    auto ix = ir.ConvertFToU(32, x), iy = ir.ConvertFToU(32, y);
    auto select = ir.BitwiseAnd(IR::U32{ir.BitwiseXor(ix, iy)}, ir.Imm32(1U));
    auto sx = IR::F32{ir.FPMul(x, ir.Imm32(std::exp2(test.base_lod) / 32.f))},
         sy = IR::F32{ir.FPMul(y, ir.Imm32(std::exp2(test.base_lod) / 32.f))};
    auto layer = ir.ConvertUToF(32, 32, ir.BitwiseAnd(ix, ir.Imm32(1U)));
    IR::Value coords;
    switch (test.type) {
    case AmdGpu::ImageType::Color1D:
        coords = sx;
        break;
    case AmdGpu::ImageType::Color1DArray:
        coords = ir.CompositeConstruct(sx, layer);
        break;
    case AmdGpu::ImageType::Color2D:
        coords = ir.CompositeConstruct(sx, sy);
        break;
    case AmdGpu::ImageType::Cube:
        coords =
            ir.CompositeConstruct(ir.FPAdd(sx, ir.Imm32(1.f)), ir.FPAdd(sy, ir.Imm32(1.f)), layer);
        break;
    default:
        coords = ir.CompositeConstruct(sx, sy, layer);
        break;
    }
    auto result = ir.CompositeConstruct(ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f));
    for (u32 n = 0; n < 2; ++n) {
        auto matches = ir.IEqual(select, ir.Imm32(n));
        IR::Value handle = ir.Imm32(n);
        if (guarded) {
            auto g = block.PrependNewInst(block.Instructions().end(), IR::Opcode::GuardedResource,
                                          {handle, matches});
            handle = IR::Value{&*g};
        }
        IR::F32 bias = test.bias == 0 ? IR::F32{} : ir.Imm32(test.bias);
        auto color =
            test.depth ? ir.ImageSampleDrefImplicitLod(handle, coords, ir.Imm32(.4f), bias, {}, {})
                       : ir.ImageSampleImplicitLod(handle, coords, bias, {}, {});
        std::array<IR::Value, 4> c;
        for (u32 k = 0; k < 4; ++k)
            c[k] =
                ir.Select(matches, ir.CompositeExtract(color, k), ir.CompositeExtract(result, k));
        result = ir.CompositeConstruct(c[0], c[1], c[2], c[3]);
    }
    for (u32 k = 0; k < 4; ++k)
        ir.SetAttribute(IR::Attribute::RenderTarget0, IR::F32{ir.CompositeExtract(result, k)}, k);
    Profile p{};
    p.supported_spirv = 0x10500;
    p.subgroup_size = 64;
    Optimization::CollectShaderInfoPass(program, p);
    RuntimeInfo r{};
    r.Initialize(HwStage::Fragment, SwStage::Fragment);
    r.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(p, r, program, bindings);
}
int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("dynamic-lod-probe");
    Window w;
    auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(w, 0, false, false, driver);
    auto d = instance.GetDevice();
    auto mp = instance.GetMemoryProperties();
    auto allocate = [&](vk::MemoryRequirements r, vk::MemoryPropertyFlags flags) {
        for (u32 i = 0; i < mp.memoryTypeCount; ++i)
            if ((r.memoryTypeBits & (1U << i)) &&
                (mp.memoryTypes[i].propertyFlags & flags) == flags)
                return Vulkan::Check(
                    d.allocateMemoryUnique({.allocationSize = r.size, .memoryTypeIndex = i}));
        throw std::runtime_error("memory type");
    };
    auto output =
        Vulkan::Check(d.createImageUnique({.imageType = vk::ImageType::e2D,
                                           .format = vk::Format::eR32G32B32A32Sfloat,
                                           .extent = {8, 8, 1},
                                           .mipLevels = 1,
                                           .arrayLayers = 1,
                                           .samples = vk::SampleCountFlagBits::e1,
                                           .tiling = vk::ImageTiling::eOptimal,
                                           .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                    vk::ImageUsageFlagBits::eTransferSrc}));
    auto omem = allocate(d.getImageMemoryRequirements(*output), {});
    Vulkan::Check(d.bindImageMemory(*output, *omem, 0));
    auto oview = Vulkan::Check(d.createImageViewUnique(
        {.image = *output,
         .viewType = vk::ImageViewType::e2D,
         .format = vk::Format::eR32G32B32A32Sfloat,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
    auto buf = Vulkan::Check(
        d.createBufferUnique({.size = 1024, .usage = vk::BufferUsageFlagBits::eTransferDst}));
    auto bmem = allocate(d.getBufferMemoryRequirements(*buf),
                         vk::MemoryPropertyFlagBits::eHostVisible |
                             vk::MemoryPropertyFlagBits::eHostCoherent);
    Vulkan::Check(d.bindBufferMemory(*buf, *bmem, 0));
    auto mapped = static_cast<float*>(Vulkan::Check(d.mapMemory(*bmem, 0, 1024)));
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < 3; ++i)
        bindings[i] = {.binding = i,
                       .descriptorType = i == 2 ? vk::DescriptorType::eSampler
                                                : vk::DescriptorType::eSampledImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eFragment};
    auto dl = Vulkan::Check(
        d.createDescriptorSetLayoutUnique({.bindingCount = 3, .pBindings = bindings.data()}));
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(PushData)};
    auto pl = Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                          .pSetLayouts = &*dl,
                                                          .pushConstantRangeCount = 1,
                                                          .pPushConstantRanges = &push}));
    std::array counts{vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 2},
                      vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1}};
    auto dp = Vulkan::Check(d.createDescriptorPoolUnique(
        {.maxSets = 1, .poolSizeCount = 2, .pPoolSizes = counts.data()}));
    auto sets = Vulkan::Check(d.allocateDescriptorSets(
        {.descriptorPool = *dp, .descriptorSetCount = 1, .pSetLayouts = &*dl}));
    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto cmds = Vulkan::Check(d.allocateCommandBuffers(
        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
    auto cmd = cmds[0];
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    auto submit = [&] {
        Vulkan::Check(cmd.end());
        Vulkan::Check(instance.GetGraphicsQueue().submit(
            vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, *fence));
        if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
            throw std::runtime_error("GPU timeout");
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    };
    auto module = [&](std::vector<u32> words, const std::string& name) {
        std::ofstream out(std::string(argv[2]) + "/lod-" + name + ".spv", std::ios::binary);
        out.write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
        return Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = words.size() * 4, .pCode = words.data()}));
    };
    auto vs = module(Vertex(), "vertex");
    const float max_bias = instance.MaxSamplerLodBias();
    printf("maxSamplerLodBias=%g\n", max_bias);
    const Case cases[] = {{"2d", AmdGpu::ImageType::Color2D, 0},
                          {"2d-bias-plus", AmdGpu::ImageType::Color2D, 1},
                          {"2d-bias-minus", AmdGpu::ImageType::Color2D, -1},
                          {"1d", AmdGpu::ImageType::Color1D, 0},
                          {"1d-array", AmdGpu::ImageType::Color1DArray, 1},
                          {"2d-array", AmdGpu::ImageType::Color2DArray, 0},
                          {"cube", AmdGpu::ImageType::Cube, -1},
                          {"depth", AmdGpu::ImageType::Color2D, 0, true},
                          {"depth-bias", AmdGpu::ImageType::Color2D, 1, true},
                          {"bias-positive-limit", AmdGpu::ImageType::Color2D, 3, false,
                           max_bias - 1, 2 - max_bias, true},
                          {"bias-negative-limit", AmdGpu::ImageType::Color2D, -3, false,
                           1 - max_bias, 2 + max_bias, true},
                          {"bias-positive-cancel", AmdGpu::ImageType::Color2D, -2, false, 2},
                          {"bias-negative-cancel", AmdGpu::ImageType::Color2D, 2, false, -2}};
    u32 checks = 0, failures = 0;
    for (const auto& test : cases) {
        // The bound/unbound reference itself violates the combined-bias clamp on
        // the pinned A740 Turnip driver. Keep the explicit diagnostic reproducible,
        // without treating that independent failure as a derivative-guard regression.
        if (test.limit_diagnostic && !std::getenv("SHADPS4_LOD_LIMIT_TEST"))
            continue;
        bool one_d =
            test.type == AmdGpu::ImageType::Color1D || test.type == AmdGpu::ImageType::Color1DArray;
        bool array = test.type == AmdGpu::ImageType::Color1DArray ||
                     test.type == AmdGpu::ImageType::Color2DArray ||
                     test.type == AmdGpu::ImageType::Cube;
        auto aspect =
            test.depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
        auto format = test.depth ? vk::Format::eD32Sfloat : vk::Format::eR32G32B32A32Sfloat;
        std::array<vk::UniqueImage, 2> images;
        std::array<vk::UniqueDeviceMemory, 2> mems;
        std::array<vk::UniqueImageView, 2> views;
        Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
        for (u32 i = 0; i < 2; ++i) {
            images[i] =
                Vulkan::Check(d.createImageUnique({.imageType = vk::ImageType::e2D,
                                                   .format = format,
                                                   .extent = {32, one_d ? 1U : 32U, 1},
                                                   .mipLevels = 6,
                                                   .arrayLayers = array ? 6U : 1U,
                                                   .samples = vk::SampleCountFlagBits::e1,
                                                   .tiling = vk::ImageTiling::eOptimal,
                                                   .usage = vk::ImageUsageFlagBits::eSampled |
                                                            vk::ImageUsageFlagBits::eTransferDst}));
            mems[i] = allocate(d.getImageMemoryRequirements(*images[i]), {});
            Vulkan::Check(d.bindImageMemory(*images[i], *mems[i], 0));
            vk::ImageSubresourceRange range{aspect, 0, 6, 0, array ? 6U : 1U};
            views[i] = Vulkan::Check(d.createImageViewUnique(
                {.image = *images[i],
                 .viewType = array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
                 .format = format,
                 .subresourceRange = range}));
            vk::ImageMemoryBarrier bar{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                                       .oldLayout = vk::ImageLayout::eUndefined,
                                       .newLayout = vk::ImageLayout::eGeneral,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = *images[i],
                                       .subresourceRange = range};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, bar);
            for (u32 m = 0; m < 6; ++m)
                for (u32 layer = 0; layer < (array ? 6U : 1U); ++layer) {
                    vk::ImageSubresourceRange r{aspect, m, 1, layer, 1};
                    if (test.depth)
                        cmd.clearDepthStencilImage(*images[i], vk::ImageLayout::eGeneral,
                                                   {.depth = float(m + i) / 8}, r);
                    else
                        cmd.clearColorImage(
                            *images[i], vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{.float32 = std::array{float(m) / 8, float(i),
                                                                      float(layer) / 8, 1.f}},
                            r);
                }
            bar.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            bar.dstAccessMask = vk::AccessFlagBits::eShaderRead;
            bar.oldLayout = vk::ImageLayout::eGeneral;
            bar.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, bar);
        }
        submit();
        auto sampler =
            Vulkan::Check(d.createSamplerUnique({.magFilter = vk::Filter::eNearest,
                                                 .minFilter = vk::Filter::eNearest,
                                                 .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                                 .mipLodBias = test.sampler_bias,
                                                 .compareEnable = test.depth,
                                                 .compareOp = vk::CompareOp::eLessOrEqual,
                                                 .maxLod = 5}));
        for (u32 i = 0; i < 3; ++i) {
            vk::DescriptorImageInfo ii{.sampler = *sampler,
                                       .imageView = *views[i % 2],
                                       .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
            d.updateDescriptorSets(
                vk::WriteDescriptorSet{.dstSet = sets[0],
                                       .dstBinding = i,
                                       .descriptorCount = 1,
                                       .descriptorType = bindings[i].descriptorType,
                                       .pImageInfo = &ii},
                {});
        }
        std::array<float, 256> reference{};
        u32 before = failures;
        for (bool guarded : {false, true}) {
            auto fs = module(Fragment(test, guarded),
                             std::string(test.name) + (guarded ? "-guarded" : "-reference"));
            std::array stages{
                vk::PipelineShaderStageCreateInfo{
                    .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
                vk::PipelineShaderStageCreateInfo{
                    .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
            vk::PipelineVertexInputStateCreateInfo vi{};
            vk::PipelineInputAssemblyStateCreateInfo ia{.topology =
                                                            vk::PrimitiveTopology::eTriangleList};
            vk::Viewport viewport{0, 0, 8, 8, 0, 1};
            vk::Rect2D scissor{{0, 0}, {8, 8}};
            vk::PipelineViewportStateCreateInfo vp{.viewportCount = 1,
                                                   .pViewports = &viewport,
                                                   .scissorCount = 1,
                                                   .pScissors = &scissor};
            vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill,
                                                            .lineWidth = 1};
            vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                          vk::SampleCountFlagBits::e1};
            vk::PipelineColorBlendAttachmentState blend{
                .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                  vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
            vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &blend};
            vk::Format of = vk::Format::eR32G32B32A32Sfloat;
            vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                      .pColorAttachmentFormats = &of};
            auto pipeline =
                Vulkan::Check(d.createGraphicsPipelineUnique({}, {.pNext = &rendering,
                                                                  .stageCount = 2,
                                                                  .pStages = stages.data(),
                                                                  .pVertexInputState = &vi,
                                                                  .pInputAssemblyState = &ia,
                                                                  .pViewportState = &vp,
                                                                  .pRasterizationState = &raster,
                                                                  .pMultisampleState = &ms,
                                                                  .pColorBlendState = &cb,
                                                                  .layout = *pl}));
            Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
            vk::ImageMemoryBarrier bar{
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *output,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, bar);
            vk::RenderingAttachmentInfo attachment{.imageView = *oview,
                                                   .imageLayout =
                                                       vk::ImageLayout::eColorAttachmentOptimal,
                                                   .loadOp = vk::AttachmentLoadOp::eClear,
                                                   .storeOp = vk::AttachmentStoreOp::eStore};
            cmd.beginRendering({.renderArea = scissor,
                                .layerCount = 1,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &attachment});
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pl, 0, sets, {});
            cmd.draw(3, 1, 0, 0);
            cmd.endRendering();
            bar.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
            bar.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            bar.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
            bar.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, bar);
            cmd.copyImageToBuffer(
                *output, vk::ImageLayout::eTransferSrcOptimal, *buf,
                vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                    .imageExtent = {8, 8, 1}});
            submit();
            for (u32 pix = 0; pix < 64; ++pix) {
                u32 x = pix % 8, y = pix / 8, index = (x ^ y) & 1;
                float mip = std::clamp(
                    test.base_lod + std::clamp(test.bias + test.sampler_bias, -max_bias, max_bias),
                    0.f, 5.f);
                std::array<float, 4> expected =
                    test.depth
                        ? std::array{.4f <= (mip + index) / 8 ? 1.f : 0.f, 0.f, 0.f, 0.f}
                        : std::array{mip / 8, float(index), array ? float(x & 1) / 8 : 0.f, 1.f};
                for (u32 c = 0; c < 4; ++c) {
                    ++checks;
                    float value = mapped[pix * 4 + c];
                    if (!std::isfinite(value) || std::abs(value - expected[c]) > 1e-5f) {
                        if (failures < 10)
                            printf("%s guarded=%d pix=%u c=%u got=%g expected=%g\n", test.name,
                                   guarded, pix, c, value, expected[c]);
                        ++failures;
                    }
                    if (guarded) {
                        ++checks;
                        if (std::abs(value - reference[pix * 4 + c]) > 1e-5f)
                            ++failures;
                    } else
                        reference[pix * 4 + c] = value;
                }
            }
        }
        printf("%s failures=%u\n", test.name, failures - before);
        fflush(stdout);
    }
    d.unmapMemory(*bmem);
    printf("DYNAMIC_LOD %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    int result = Run(argc, argv);
    Common::Log::Shutdown();
    return result;
}
