// SPDX-License-Identifier: GPL-2.0-or-later
// Execute production DS_SWIZZLE lowering with varying lane sources and all quad permutations.
#include <cmath>
#include <cstdio>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/serdes.h"
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
static constexpr u32 Cases = 5 + 256 + 2;
static u32 SourceLane(u32 lane, u32 mode) {
    if (mode < 5)
        return lane ^ (16u >> mode);
    if (mode < 261)
        return (lane & ~3u) | (((mode - 5) >> ((lane & 3) * 2)) & 3);
    return mode == 261 ? 31 : 63;
}
static std::vector<u32> Fragment(const Profile& profile, u32 mode) {
    Info info{};
    info.stage = Stage::Fragment;
    info.l_stage = LogicalStage::Fragment;
    info.uses_group_ballot = true; // The test separately checks source-lane activity.
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
    const auto lane = ir.LaneId();
    ir.SetVectorReg(IR::VectorReg::V2, ir.IAdd(lane, ir.Imm32(17u)));
    IR::U32 target;
    if (mode < 261) {
        const u32 off = mode < 5 ? (0x001fu | ((16u >> mode) << 10)) : (0x8000u | (mode - 5));
        // SICI DS_SWIZZLE_B32: source v2, destination v3.
        const std::array<u32, 2> words{0xd8000000u | (53u << 18) | off, 2u | (3u << 24)};
        Gcn::GcnCodeSlice slice(words.data(), words.data() + 2);
        Gcn::GcnDecodeContext decoder;
        const auto inst = decoder.decodeInstruction(slice);
        ASSERT(inst.opcode == Gcn::Opcode::DS_SWIZZLE_B32);
        t.TranslateInstruction(inst);
        target =
            mode < 5
                ? IR::U32{ir.BitwiseXor(lane, ir.Imm32(16u >> mode))}
                : IR::U32{ir.BitwiseOr(
                      ir.BitwiseAnd(lane, ir.Imm32(~3u)),
                      ir.BitFieldExtract(ir.Imm32(mode - 5),
                                         ir.IMul(ir.BitwiseAnd(lane, ir.Imm32(3u)), ir.Imm32(2u)),
                                         ir.Imm32(2u)))};
    } else {
        target = ir.Imm32(mode == 261 ? 31u : 63u);
        ir.SetVectorReg(IR::VectorReg::V3, ir.ReadLane(ir.GetVectorReg(IR::VectorReg::V2), target));
    }
    const auto ballot = ir.Ballot(ir.Imm1(true));
    IR::U32 mask{ir.CompositeExtract(ballot, 0)};
    for (u32 word = 1; word < 4; ++word)
        mask =
            IR::U32{ir.Select(ir.IEqual(ir.ShiftRightLogical(target, ir.Imm32(5u)), ir.Imm32(word)),
                              ir.CompositeExtract(ballot, word), mask)};
    const auto active =
        ir.INotEqual(ir.BitwiseAnd(ir.ShiftRightLogical(mask, ir.BitwiseAnd(target, ir.Imm32(31u))),
                                   ir.Imm32(1u)),
                     ir.Imm32(0u));
    auto normalized = [&](IR::U32 value) {
        return ir.FPMul(ir.ConvertUToF(32, 32, value), ir.Imm32(1.f / 255));
    };
    ir.SetAttribute(IR::Attribute::RenderTarget0, IR::F32{normalized(lane)}, 0);
    ir.SetAttribute(IR::Attribute::RenderTarget0,
                    IR::F32{normalized(ir.GetVectorReg(IR::VectorReg::V3))}, 1);
    ir.SetAttribute(IR::Attribute::RenderTarget0,
                    IR::F32{ir.Select(active, ir.Imm32(1.f), ir.Imm32(0.f))}, 2);
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(1.f), 3);
    Optimization::SsaRewritePass(p);
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    Optimization::DeadCodeEliminationPass(p);
    Optimization::CollectShaderInfoPass(p, profile);
    Backend::Bindings bindings{};
    return EmitSPIRV(profile, rt, p, bindings);
}

static bool CheckWaveMetadata() {
    for (u32 flags = 0; flags < 4; ++flags) {
        Info input{}, restored{};
        input.uses_lane_id = flags & 1;
        input.uses_group_ballot = flags & 2;
        input.flattened_ud_buf = {31, 63, 64};
        Serialization::Archive writer;
        input.Serialize(writer);
        Serialization::Archive reader{writer.TakeOff()};
        if (!restored.Deserialize(reader) || !reader.IsEoS() ||
            restored.uses_lane_id != input.uses_lane_id ||
            restored.uses_group_ballot != input.uses_group_ballot ||
            restored.flattened_ud_buf != input.flattened_ud_buf)
            return false;
    }
    printf("WAVE_METADATA 4 combinations passed\n");
    return true;
}
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (!CheckWaveMetadata())
        return 5;
    if (argc != 3)
        return 2;
    if (std::string_view(argv[1]) == "emit-only") {
        Profile profile{};
        profile.supported_spirv = 0x10500;
        for (u32 mode = 0; mode < Cases; ++mode) {
            const auto code = Fragment(profile, mode);
            std::ofstream out(std::string(argv[2]) + "/swizzle-" + std::to_string(mode) + ".spv",
                              std::ios::binary);
            out.write((const char*)code.data(), code.size() * 4);
            if (!out)
                return 4;
        }
        return 0;
    }
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("swizzle-probe");
    Window window;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    printf("SUBGROUP default=%u fragment64=%u\n", instance.SubgroupSize(),
           instance.IsSubgroupSize64Supported(vk::ShaderStageFlagBits::eFragment));
    Vulkan::Scheduler scheduler(instance);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    auto d = instance.GetDevice();
    constexpr u32 W = 32, H = 32, Layers = 1;
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
    for (u32 mode = 0; mode < Cases; ++mode) {
        auto code = Fragment(profile, mode);
        std::ofstream out(std::string(argv[2]) + "/swizzle-" + std::to_string(mode) + ".spv",
                          std::ios::binary);
        out.write((const char*)code.data(), code.size() * 4);
        auto fs = Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize =
                                                                                 64};
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main"},
            vk::PipelineShaderStageCreateInfo{
                .pNext = instance.IsSubgroupSize64Supported(vk::ShaderStageFlagBits::eFragment)
                             ? &subgroup
                             : nullptr,
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = *fs,
                .pName = "main"}};
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
        const vk::PipelineViewportStateCreateInfo viewport_state{
            .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
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
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
        cmd.copyImageToBuffer(
            image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, *buffer,
            vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, Layers},
                                .imageExtent = {W, H, 1}});
        const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                      .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
        scheduler.Finish();
        const auto before = failures;
        if (mode >= 261) {
            printf("fixed mode=%u first_rgba=%u,%u,%u,%u expected=%u\n", mode,
                   bytes[0], bytes[1], bytes[2], bytes[3], SourceLane(bytes[0], mode) + 17);
        }
        unsigned active_sources = 0;
        for (u32 pixel = 0; pixel < W * H; ++pixel) {
            const u32 lane = bytes[pixel * 4];
            checks += 2;
            failures += lane >= 64;
            failures += bytes[pixel * 4 + 3] != 255;
            if (bytes[pixel * 4 + 2] == 255) {
                ++active_sources;
                ++checks;
                failures +=
                    std::abs(int(bytes[pixel * 4 + 1]) - int(SourceLane(lane, mode) + 17)) > 1;
            }
        }
        ++checks;
        failures += active_sources < W * H / 2;
        printf("mode=%u active_sources=%u failures=%u\n", mode, active_sources, failures - before);
    }
    d.unmapMemory(*mem);
    d.destroyShaderModule(vs);
    printf("SWIZZLE_GPU %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
