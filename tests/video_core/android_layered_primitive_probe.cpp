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

static std::vector<u32> Vertex(Info info, const Profile& profile, bool quad, bool clip) {
    RuntimeInfo runtime{};
    runtime.stage = Stage::Vertex;
    runtime.vs_info.tess_emulated_primitive = true;
    runtime.depth_range.enabled = clip;
    runtime.depth_range.clip_near = clip;
    runtime.depth_range.clip_far = clip;
    for (auto& viewport : runtime.depth_range.viewports)
        viewport = AmdGpu::MakeDepthRangeTransform(1.f, 0.f, 0.f, 1.f);
    Shader::Backend::Bindings bindings{};
    EmitContext c(profile, runtime, info, bindings);
    c.AddCapability(spv::Capability::DrawParameters);
    c.AddCapability(spv::Capability::Int8);
    c.AddCapability(spv::Capability::Int16);
    const auto main =
        c.OpFunction(c.void_id, spv::FunctionControlMask::MaskNone, c.TypeFunction(c.void_id));
    c.AddLabel();
    EmitPrologue(c);
    const auto f = [&](float v) { return c.Constant(c.F32[1], v); };
    const auto vid = c.OpLoad(c.U32[1], c.vertex_index);
    const auto iid = c.OpLoad(c.U32[1], c.instance_id);
    const auto is = [&](u32 v) { return c.OpIEqual(c.U1[1], vid, c.ConstU32(v)); };
    // Rect: bottom-left, bottom-right, top-left. Quad: cyclic corners.
    const auto right = quad ? c.OpLogicalOr(c.U1[1], is(1), is(2)) : is(1);
    const auto top = quad ? c.OpLogicalOr(c.U1[1], is(2), is(3)) : is(2);
    c.OpStore(c.output_position,
              c.OpCompositeConstruct(c.F32[4], c.OpSelect(c.F32[1], right, f(1), f(-1)),
                                     c.OpSelect(c.F32[1], top, f(1), f(-1)), f(.5f), f(1)));
    const auto first = c.OpIEqual(c.U1[1], iid, c.ConstU32(0U));
    const auto color = c.OpCompositeConstruct(c.F32[4], c.OpSelect(c.F32[1], first, f(1), f(0)),
                                              c.OpSelect(c.F32[1], first, f(0), f(1)), f(0), f(1));
    for (u32 i : {0U, 7U, 31U})
        c.OpStore(c.output_params[i].id, color);
    for (auto attr : {IR::Attribute::RenderTargetIndex, IR::Attribute::ViewportIndex})
        if (info.stores.GetAny(attr))
            EmitSetAttribute(c, attr, c.OpBitcast(c.F32[1], iid), 0);
    EmitEpilogue(c);
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", c.interfaces);
    return c.Assemble();
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4)
        return 2;
    const bool negative = argc == 4 && std::string_view(argv[3]) == "omit-bridge";
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("layered-primitive-probe");
    Window window;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    Vulkan::Scheduler scheduler(instance);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    auto d = instance.GetDevice();
    constexpr u32 W = 16, H = 8, Layers = 2;
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
    const auto fs = Vulkan::Compile(R"(#version 450
layout(location=0) in vec4 color;
layout(location=0) out vec4 result;
void main() { result = color; }
)",
                                    vk::ShaderStageFlagBits::eFragment, d);
    Profile profile{};
    profile.supported_spirv = 0x10500;
    unsigned checks = 0, failures = 0;
    for (bool quad : {false, true})
        for (u32 mask = 0; mask < 4; ++mask)
            for (bool clip : {false, true}) {
                Info info{};
                info.stage = Stage::Vertex;
                info.l_stage = LogicalStage::Vertex;
                for (u32 loc : {0U, 7U, 31U})
                    for (u32 c = 0; c < 4; ++c)
                        info.stores.Set(IR::Attribute::Param0 + loc, c);
                if (mask & 1)
                    info.stores.Set(IR::Attribute::RenderTargetIndex, 0);
                if (mask & 2)
                    info.stores.Set(IR::Attribute::ViewportIndex, 0);
                auto builtins =
                    negative ? AuxiliaryTessBuiltins{} : AuxiliaryBuiltinLocations(info, profile);
                const auto locations = AuxiliaryVaryingLocations(info, profile);
                const auto type = quad ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
                std::array codes{Vertex(info, profile, quad, clip),
                                 EmitAuxilaryTessShader(type, locations, clip, builtins),
                                 EmitAuxilaryTessShader(AuxShaderType::PassthroughTES, locations,
                                                        clip, builtins)};
                std::array<vk::UniqueShaderModule, 3> modules;
                std::array<vk::PipelineShaderStageCreateInfo, 4> stages;
                constexpr std::array bits{vk::ShaderStageFlagBits::eVertex,
                                          vk::ShaderStageFlagBits::eTessellationControl,
                                          vk::ShaderStageFlagBits::eTessellationEvaluation};
                for (u32 i = 0; i < 3; ++i) {
                    auto name = std::string(argv[2]) + "/layered-" + std::to_string(quad) + "-" +
                                std::to_string(mask) + "-" + std::to_string(clip) + "-" +
                                std::to_string(i) + ".spv";
                    std::ofstream file(name, std::ios::binary);
                    file.write(reinterpret_cast<const char*>(codes[i].data()), codes[i].size() * 4);
                    modules[i] = Vulkan::Check(d.createShaderModuleUnique(
                        {.codeSize = codes[i].size() * 4, .pCode = codes[i].data()}));
                    stages[i] = {.stage = bits[i], .module = *modules[i], .pName = "main"};
                }
                stages[3] = {
                    .stage = vk::ShaderStageFlagBits::eFragment, .module = fs, .pName = "main"};
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
                vi.type = AmdGpu::ImageType::Color2DArray;
                vi.format = image_info.pixel_format;
                vi.range.extent = image_info.resources;
                const auto view = image.FindView(vi).image_view.get();
                const vk::PipelineVertexInputStateCreateInfo vertex{};
                const vk::PipelineInputAssemblyStateCreateInfo assembly{
                    .topology = vk::PrimitiveTopology::ePatchList};
                const vk::PipelineTessellationStateCreateInfo tess{.patchControlPoints =
                                                                       quad ? 4U : 3U};
                const float width = (mask & 2) ? W / 2 : W;
                const std::array viewport{vk::Viewport{0, 0, width, H, 0, 1},
                                          vk::Viewport{width, 0, width, H, 0, 1}};
                const std::array scissor{vk::Rect2D{{0, 0}, {u32(width), H}},
                                         vk::Rect2D{{s32(width), 0}, {u32(width), H}}};
                const u32 count = (mask & 2) ? 2 : 1;
                const vk::PipelineViewportStateCreateInfo viewport_state{
                    .viewportCount = count,
                    .pViewports = viewport.data(),
                    .scissorCount = count,
                    .pScissors = scissor.data()};
                const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1};
                const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                                    vk::SampleCountFlagBits::e1};
                const vk::PipelineColorBlendAttachmentState blend{.colorWriteMask =
                                                                      vk::ColorComponentFlags(15)};
                const vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount = 1,
                                                                     .pAttachments = &blend};
                const vk::PipelineRenderingCreateInfo rendering{
                    .colorAttachmentCount = 1, .pColorAttachmentFormats = &image_info.pixel_format};
                auto pipeline = Vulkan::Check(
                    d.createGraphicsPipelineUnique({}, {.pNext = &rendering,
                                                        .stageCount = 4,
                                                        .pStages = stages.data(),
                                                        .pVertexInputState = &vertex,
                                                        .pInputAssemblyState = &assembly,
                                                        .pTessellationState = &tess,
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
                cmd.draw(quad ? 4 : 3, 2, 0, 0);
                cmd.endRendering();
                image.Transit(vk::ImageLayout::eTransferSrcOptimal,
                              vk::AccessFlagBits2::eTransferRead, {});
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
                printf("extent=%ux%u first=%u,%u,%u,%u last=%u,%u,%u,%u layer1=%u,%u,%u,%u\n",
                       image.HostExtent().width, image.HostExtent().height, bytes[0], bytes[1],
                       bytes[2], bytes[3], bytes[W * H * 4 - 4], bytes[W * H * 4 - 3],
                       bytes[W * H * 4 - 2], bytes[W * H * 4 - 1], bytes[W * H * 4],
                       bytes[W * H * 4 + 1], bytes[W * H * 4 + 2], bytes[W * H * 4 + 3]);
                const auto before = failures;
                for (u32 l = 0; l < Layers; ++l)
                    for (u32 y = 0; y < H; ++y)
                        for (u32 x = 0; x < W; ++x) {
                            std::array<u8, 4> expected{};
                            for (u32 eye = 0; eye < 2; ++eye) {
                                const u32 layer = (mask & 1) ? eye : 0;
                                if (l == layer && (!(mask & 2) || x / (W / 2) == eye))
                                    expected = {u8(eye == 0 ? 255 : 0), u8(eye == 1 ? 255 : 0), 0,
                                                255};
                            }
                            for (u32 c = 0; c < 4; ++c) {
                                ++checks;
                                if (bytes[((l * H + y) * W + x) * 4 + c] != expected[c])
                                    ++failures;
                            }
                        }
                printf("quad=%u mask=%u clip=%u failures=%u\n", quad, mask, clip,
                       failures - before);
            }
    d.unmapMemory(*mem);
    d.destroyShaderModule(fs);
    printf("LAYERED_PRIMITIVE negative_control=%u %u checks / %u failures\n", negative, checks,
           failures);
    return failures ? 1 : 0;
}
