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
static std::vector<u32> Vertex(Info info, const Profile& profile) {
    RuntimeInfo runtime{};
    runtime.stage = Stage::Vertex;
    Shader::Backend::Bindings bindings{};
    EmitContext c(profile, runtime, info, bindings);
    c.AddCapability(spv::Capability::DrawParameters);
    c.AddCapability(spv::Capability::Int8);
    c.AddCapability(spv::Capability::Int16);
    auto main =
        c.OpFunction(c.void_id, spv::FunctionControlMask::MaskNone, c.TypeFunction(c.void_id));
    c.AddLabel();
    EmitPrologue(c);
    auto value = c.OpCompositeConstruct(c.F32[4], c.f32_zero_value, c.f32_zero_value,
                                        c.f32_zero_value, c.Constant(c.F32[1], 1.f));
    c.OpStore(c.output_position, value);
    for (u32 i = 0; i < IR::NumParams; ++i)
        if (info.stores.GetAny(IR::Attribute::Param0 + i))
            c.OpStore(c.output_params[i].id, value);
    EmitEpilogue(c);
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", c.interfaces);
    return c.Assemble();
}
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[1]);
    Common::Log::Setup("aux-probe");
    Window window;
    Vulkan::Instance instance(window, 0, false, false, Vulkan::LoadAndroidSystemDriver());
    auto d = instance.GetDevice();
    auto layout = Vulkan::Check(d.createPipelineLayoutUnique({}));
    unsigned checks{}, failures{};
    Profile profile{};
    profile.supported_spirv = 0x10500;
    const std::array<std::vector<u32>, 4> cases{{{}, {0}, {0, 7, 31}, {2, 5}}};
    for (const auto& locations : cases) {
        Info info{};
        info.stage = Stage::Vertex;
        info.l_stage = LogicalStage::Vertex;
        for (u32 location : locations)
            info.stores.Set(IR::Attribute::Param0 + location, 0);
        auto actual = AuxiliaryVaryingLocations(info, profile);
        ++checks;
        if (actual != locations)
            ++failures;
        for (auto type : {AuxShaderType::RectListTCS, AuxShaderType::QuadListTCS}) {
            std::array<std::vector<u32>, 3> codes{
                Vertex(info, profile), EmitAuxilaryTessShader(type, actual),
                EmitAuxilaryTessShader(AuxShaderType::PassthroughTES, actual)};
            std::array<vk::UniqueShaderModule, 3> modules;
            std::array<vk::PipelineShaderStageCreateInfo, 3> stages;
            constexpr std::array bits{vk::ShaderStageFlagBits::eVertex,
                                      vk::ShaderStageFlagBits::eTessellationControl,
                                      vk::ShaderStageFlagBits::eTessellationEvaluation};
            for (u32 i = 0; i < 3; ++i) {
                modules[i] = Vulkan::Check(d.createShaderModuleUnique(
                    {.codeSize = codes[i].size() * 4, .pCode = codes[i].data()}));
                stages[i] = {.stage = bits[i], .module = *modules[i], .pName = "main"};
            }
            vk::PipelineVertexInputStateCreateInfo vi{};
            vk::PipelineInputAssemblyStateCreateInfo ia{.topology =
                                                            vk::PrimitiveTopology::ePatchList};
            vk::PipelineTessellationStateCreateInfo ts{
                .patchControlPoints = type == AuxShaderType::RectListTCS ? 3U : 4U};
            vk::Viewport viewport{0, 0, 8, 8, 0, 1};
            vk::Rect2D scissor{{0, 0}, {8, 8}};
            vk::PipelineViewportStateCreateInfo vp{.viewportCount = 1,
                                                   .pViewports = &viewport,
                                                   .scissorCount = 1,
                                                   .pScissors = &scissor};
            vk::PipelineRasterizationStateCreateInfo rs{.polygonMode = vk::PolygonMode::eFill,
                                                        .lineWidth = 1};
            vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                          vk::SampleCountFlagBits::e1};
            vk::PipelineDepthStencilStateCreateInfo ds{};
            vk::PipelineColorBlendStateCreateInfo cb{};
            vk::PipelineRenderingCreateInfo rendering{};
            auto [result, pipeline] =
                d.createGraphicsPipelineUnique({}, {.pNext = &rendering,
                                                    .stageCount = 3,
                                                    .pStages = stages.data(),
                                                    .pVertexInputState = &vi,
                                                    .pInputAssemblyState = &ia,
                                                    .pTessellationState = &ts,
                                                    .pViewportState = &vp,
                                                    .pRasterizationState = &rs,
                                                    .pMultisampleState = &ms,
                                                    .pDepthStencilState = &ds,
                                                    .pColorBlendState = &cb,
                                                    .layout = *layout});
            ++checks;
            if (result != vk::Result::eSuccess)
                ++failures;
            printf("locations=%zu type=%u result=%s\n", actual.size(), u32(type),
                   vk::to_string(result).c_str());
        }
    }
    printf("AUX_INTERFACE %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
