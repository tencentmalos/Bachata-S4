// SPDX-License-Identifier: GPL-2.0-or-later
// Decode GCN paired LDS, lower proven lane-private storage, execute fragment SPIR-V.
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
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
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

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"
static std::vector<u32> Fragment(const Profile& profile, u32 mode, bool scalar) {
    Info info{};
    info.stage = Stage::Fragment;
    info.l_stage = LogicalStage::Fragment;
    RuntimeInfo rt{};
    rt.Initialize(Stage::Fragment);
    rt.num_allocated_vgprs = 16;
    rt.fs_info.color_buffers[0].num_format = AmdGpu::NumberFormat::Unorm;
    Pools pools;
    IR::Program p{info};
    auto* b = pools.block_pool.Create(pools.inst_pool);
    p.blocks = {b};
    p.post_order_blocks = {b};
    p.syntax_list.emplace_back();
    p.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    p.syntax_list.back().data.block = b;
    p.syntax_list.emplace_back();
    p.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    Gcn::Translator t(info, rt, profile);
    t.EmitPrologue(b);
    IR::IREmitter ir{*b};
    auto addr = ir.IMul(ir.BitFieldExtract(ir.LaneId(), ir.Imm32(0), ir.Imm32(24)),
                        ir.Imm32(mode < 3 ? 12u : 4u));
    ir.SetVectorReg(IR::VectorReg::V1, addr);
    for (u32 c = 0; c < 2; ++c)
        ir.SetVectorReg(IR::VectorReg(2 + c),
                        ir.BitCast<IR::U32>(IR::F32{ir.FPMul(
                            ir.GetAttribute(IR::Attribute::FragCoord, c), ir.Imm32(1.f / 16))}));
    const std::array offsets{std::pair{0u, 1u}, std::pair{2u, 192u}, std::pair{193u, 194u},
                             std::pair{3u, 255u}, std::pair{7u, 7u}};
    const auto [o0, o1] = offsets[mode];
    const bool stride = mode >= 3;
    auto ds = [&](u32 op, u32 a, u32 c, u32 dst) {
        const std::array<u32, 2> w{0xd8000000u | (op << 18) | (c << 8) | a,
                                   1u | (2u << 8) | (3u << 16) | (dst << 24)};
        Gcn::GcnCodeSlice sl(w.data(), w.data() + 2);
        Gcn::GcnDecodeContext dec;
        t.TranslateInstruction(dec.decodeInstruction(sl));
    };
    ds(stride ? 15 : 14, o0, o1, 0);
    ir.SetVectorReg(IR::VectorReg::V2, ir.Imm32(0u));
    ir.SetVectorReg(IR::VectorReg::V3, ir.Imm32(0u));
    if (scalar) {
        const auto a = o0 * 4 * (stride ? 64 : 1), c = o1 * 4 * (stride ? 64 : 1);
        ds(54, a & 255, a >> 8, 4);
        ds(54, c & 255, c >> 8, 5);
    } else
        ds(stride ? 56 : 55, o0, o1, 4);
    for (u32 c = 0; c < 2; ++c)
        ir.SetAttribute(IR::Attribute::RenderTarget0,
                        ir.BitCast<IR::F32>(ir.GetVectorReg(IR::VectorReg(4 + c))), c);
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(.5f), 2);
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(1.f), 3);
    Optimization::SsaRewritePass(p);
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    ASSERT(Optimization::FragmentLdsPass(p));
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    Optimization::DeadCodeEliminationPass(p);
    Optimization::CollectShaderInfoPass(p, profile);
    Backend::Bindings bindings{};
    return EmitSPIRV(profile, rt, p, bindings);
}
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("fragment-lds-probe");
    Window window;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    Vulkan::Scheduler scheduler(instance);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    auto d = instance.GetDevice();
    constexpr u32 W = 16, H = 16, Layers = 1;
    auto buffer = Vulkan::Check(d.createBufferUnique(
        {.size = W * H * Layers * 4, .usage = vk::BufferUsageFlagBits::eTransferDst}));
    const auto req = d.getBufferMemoryRequirements(*buffer);
    const auto mp = instance.GetMemoryProperties();
    const auto flags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 mt = 0;
    while (mt < mp.memoryTypeCount && (!(req.memoryTypeBits & (1U << mt)) ||
                                       (mp.memoryTypes[mt].propertyFlags & flags) != flags))
        ++mt;
    if (mt == mp.memoryTypeCount)
        return 3;
    auto mem =
        Vulkan::Check(d.allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = mt}));
    Vulkan::Check(d.bindBufferMemory(*buffer, *mem, 0));
    auto* bytes = static_cast<u8*>(Vulkan::Check(d.mapMemory(*mem, 0, VK_WHOLE_SIZE)));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(PushData)};
    auto layout = Vulkan::Check(
        d.createPipelineLayoutUnique({.pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
    auto vs = Vulkan::Compile(R"(#version 450
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}
)",
                              vk::ShaderStageFlagBits::eVertex, d);
    Profile profile{};
    profile.supported_spirv = 0x10500;
    unsigned checks = 0, failures = 0;
    for (u32 mode = 0; mode < 5; ++mode)
        for (bool scalar : {false, true}) {
            auto code = Fragment(profile, mode, scalar);
            std::ofstream out(std::string(argv[2]) + "/lds-" + std::to_string(mode) + "-" +
                                  std::to_string(scalar) + ".spv",
                              std::ios::binary);
            out.write((const char*)code.data(), code.size() * 4);
            auto fs = Vulkan::Check(
                d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
            const std::array stages{
                vk::PipelineShaderStageCreateInfo{
                    .stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main"},
                vk::PipelineShaderStageCreateInfo{
                    .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
            VideoCore::ImageInfo image_info{};
            image_info.type = AmdGpu::ImageType::Color2D;
            image_info.pixel_format = vk::Format::eR8G8B8A8Unorm;
            image_info.size = {W, H, 1};
            image_info.resources = {1, Layers};
            image_info.num_samples = 1;
            image_info.num_bits = 32;
            image_info.pitch = W;
            image_info.guest_size = W * H * Layers * 4;
            VideoCore::Image image(instance, scheduler, blit, views, image_info);
            VideoCore::ImageViewInfo vi{};
            vi.type = AmdGpu::ImageType::Color2D;
            vi.format = image_info.pixel_format;
            vi.range.extent = image_info.resources;
            const auto view = image.FindView(vi).image_view.get();
            const vk::PipelineVertexInputStateCreateInfo vertex{};
            const vk::PipelineInputAssemblyStateCreateInfo assembly{
                .topology = vk::PrimitiveTopology::eTriangleList};
            const vk::Viewport viewport{0, 0, W, H, 0, 1};
            const vk::Rect2D scissor{{0, 0}, {W, H}};
            const vk::PipelineViewportStateCreateInfo viewport_state{.viewportCount = 1,
                                                                     .pViewports = &viewport,
                                                                     .scissorCount = 1,
                                                                     .pScissors = &scissor};
            const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1};
            const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                                vk::SampleCountFlagBits::e1};
            const vk::PipelineColorBlendAttachmentState blend{.colorWriteMask =
                                                                  vk::ColorComponentFlags(15)};
            const vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount = 1,
                                                                 .pAttachments = &blend};
            const vk::PipelineRenderingCreateInfo rendering{
                .colorAttachmentCount = 1, .pColorAttachmentFormats = &image_info.pixel_format};
            auto pipeline =
                Vulkan::Check(d.createGraphicsPipelineUnique({}, {.pNext = &rendering,
                                                                  .stageCount = 2,
                                                                  .pStages = stages.data(),
                                                                  .pVertexInputState = &vertex,
                                                                  .pInputAssemblyState = &assembly,
                                                                  .pViewportState = &viewport_state,
                                                                  .pRasterizationState = &raster,
                                                                  .pMultisampleState = &ms,
                                                                  .pColorBlendState = &blending,
                                                                  .layout = *layout}));
            image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
                          vk::AccessFlagBits2::eColorAttachmentWrite, {});
            auto cmd = scheduler.CommandBuffer();
            const vk::RenderingAttachmentInfo color{
                .imageView = view,
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearColorValue(std::array{0.f, 0.f, 0.f, 0.f})};
            cmd.beginRendering({.renderArea = {{0, 0}, {W, H}},
                                .layerCount = Layers,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &color});
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
            cmd.draw(3, 1, 0, 0);
            cmd.endRendering();
            image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                          {});
            cmd.copyImageToBuffer(
                image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, *buffer,
                vk::BufferImageCopy{
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, Layers},
                    .imageExtent = {W, H, 1}});
            const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                          .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                          .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                          .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
            scheduler.Finish();
            const auto before = failures;
            for (u32 y = 0; y < H; ++y)
                for (u32 x = 0; x < W; ++x) {
                    std::array<int, 4> e{int(std::lround((x + .5f) / 16 * 255)),
                                         int(std::lround(((mode == 4 ? x : y) + .5f) / 16 * 255)),
                                         128, 255};
                    for (u32 c = 0; c < 4; ++c) {
                        ++checks;
                        if (std::abs(int(bytes[(y * W + x) * 4 + c]) - e[c]) > 1)
                            ++failures;
                    }
                }
            printf("mode=%u scalar=%u failures=%u\n", mode, scalar, failures - before);
        }
    d.unmapMemory(*mem);
    d.destroyShaderModule(vs);
    printf("FRAGMENT_LDS_GPU %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
