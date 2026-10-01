// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>
#include <fstream>
#include <nlohmann/json.hpp>
#include "common/path_util.h"
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/profiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_interpolation.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

struct GraphicsPipeline::CreateState {
    vk::PipelineCache pipeline_cache{};
    std::string debug_str{};
    VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
    VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
    vk::PipelineMultisampleStateCreateInfo multisampling{};
    vk::PipelineVertexInputDivisorStateCreateInfo divisor_state{};
    vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    vk::PipelineTessellationStateCreateInfo tessellation_state{};
    vk::StructureChain<vk::PipelineRasterizationStateCreateInfo,
                       vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT,
                       vk::PipelineRasterizationDepthClipStateCreateInfoEXT>
        raster_chain{};
    vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control{};
    vk::PipelineViewportStateCreateInfo viewport_info{};
    boost::container::static_vector<vk::DynamicState, 32> dynamic_states{};
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages{};
    boost::container::static_vector<vk::UniqueShaderModule, 3> aux_modules{};
    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci{};
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats{};
    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
    vk::AttachmentSampleCountInfoAMD mixed_samples{};
    vk::PipelineRenderingCreateInfo pipeline_rendering_ci{};
    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments{};
    vk::PipelineColorBlendStateCreateInfo color_blending{};
    vk::PipelineDepthStencilStateCreateInfo depth_stencil_info{};
    std::optional<PipelineCreationProbe> probe{};
    vk::GraphicsPipelineCreateInfo pipeline_info{};
    Shader::HwFragmentRuntimeInfo fs_runtime{}; ///< Failure diagnostics only.
};

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading,
    bool defer_build)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    // Everything below that the driver call reads is prepared here, on the thread that knows
    // the guest state, and owned by the pipeline; the call itself may run later elsewhere.
    create_state = std::make_unique<CreateState>();
    auto& s = *create_state;
    s.pipeline_cache = pipeline_cache;
    s.debug_str = debug_str;
    s.vertex_attributes = sdata.vertex_attributes;
    s.vertex_bindings = sdata.vertex_bindings;
    s.divisors = sdata.divisors;

    s.divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(s.divisors.size()),
        .pVertexBindingDivisors = s.divisors.data(),
    };

    s.vertex_input_info = {
        .pNext = s.divisors.empty() ? nullptr : &s.divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(s.vertex_bindings.size()),
        .pVertexBindingDescriptions = s.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(s.vertex_attributes.size()),
        .pVertexAttributeDescriptions = s.vertex_attributes.data(),
    };

    s.input_assembly = {
        .topology = LiverpoolToVK::PrimitiveType(key.prim_type),
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    s.tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    auto& rasterization = s.raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
    rasterization.depthClampEnable =
        key.depth_clamp_enable && (!key.depth_clip_enable || instance.IsDepthClipEnableSupported());
    rasterization.rasterizerDiscardEnable = false;
    rasterization.polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode);
    rasterization.lineWidth = 1.0f;
    s.raster_chain.get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>()
        .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                   ? vk::ProvokingVertexModeEXT::eFirstVertex
                                   : vk::ProvokingVertexModeEXT::eLastVertex;
    s.raster_chain.get<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>().depthClipEnable =
        key.depth_clip_enable;
    if (!instance.IsProvokingVertexSupported()) {
        s.raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        s.raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.multisampling = {
            // Global framebuffer masks are only the common format guarantee.
            // Validate the concrete attachment formats below, without reducing samples.
            .rasterizationSamples = static_cast<vk::SampleCountFlagBits>(key.num_samples),
            .sampleShadingEnable = !instance.IsMsaaDisabled() &&
                (fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena),
        };
    }
    s.multisampling = sdata.multisampling;
    // Recompute even for a cached recipe written by the old global-mask path.
    s.multisampling.rasterizationSamples = static_cast<vk::SampleCountFlagBits>(key.num_samples);

    raster_samples = s.multisampling.rasterizationSamples;
    ASSERT_MSG(u32(raster_samples) == key.num_samples,
               "Unsupported pipeline sample count: requested={} actual={}",
               key.num_samples, u32(raster_samples));
    skip_eligible = std::ranges::none_of(infos, [](const Shader::Info* info) {
        return info && (info->translation_failed ||
                        std::ranges::any_of(info->buffers,
                                            [](const auto& buffer) { return buffer.is_written; }) ||
                        std::ranges::any_of(info->images, [](const auto& image) {
                            return image.is_written || image.is_atomic;
                        }));
    });
    const auto* fragment = infos[u32(Shader::SwStage::Fragment)];
    // Coarse shading must not reduce guest-visible memory writes/atomics or
    // per-sample evaluation. Read-only storage resources remain eligible.
    requires_full_fragment_rate = s.multisampling.sampleShadingEnable || !fragment;
    if (fragment) {
        requires_full_fragment_rate |= std::ranges::any_of(
            fragment->buffers, [](const auto& resource) { return resource.is_written; });
        requires_full_fragment_rate |= std::ranges::any_of(
            fragment->images,
            [](const auto& resource) { return resource.is_written || resource.is_atomic; });
    }

    s.clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    s.viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &s.clip_control : nullptr,
    };

    s.dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (instance.IsDepthBoundsSupported()) {
        s.dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        s.dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        s.dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        s.dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!s.vertex_bindings.empty()) {
        s.dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }
    if (instance.IsPipelineFragmentShadingRateSupported()) {
        s.dynamic_states.push_back(vk::DynamicState::eFragmentShadingRateKHR);
    }

    s.dynamic_info = {
        .dynamicStateCount = static_cast<u32>(s.dynamic_states.size()),
        .pDynamicStates = s.dynamic_states.data(),
    };

    // Host-generated stages (rect/quad tessellation, discard fragment) are owned by the create
    // state; the pipeline does not need them once created.
    const auto aux_module = [&](std::span<const u32> spv) {
        s.aux_modules.emplace_back(CompileSPV(spv, device), device);
        return *s.aux_modules.back();
    };
    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    if (!preloading && profile.emulate_fragment_interpolation && fragment &&
        Shader::Backend::SPIRV::MakeSoftwareInterpolationLayout(
            *fragment, runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs)
            .needed) {
        const auto* vertex = infos[u32(Shader::SwStage::Vertex)];
        if (!vertex || infos[u32(Shader::SwStage::Geometry)] ||
            infos[u32(Shader::SwStage::TessellationControl)] ||
            infos[u32(Shader::SwStage::TessellationEval)] ||
            (key.prim_type != AmdGpu::PrimitiveType::TriangleList &&
             key.prim_type != AmdGpu::PrimitiveType::TriangleStrip))
            throw std::runtime_error("software interpolation currently requires VS-only triangles");
        const auto limits = instance.GetPhysicalDevice().getProperties().limits;
        sdata.interpolation_gs = Shader::Backend::SPIRV::EmitSoftwareInterpolationGeometry(
            *vertex, runtime_infos[u32(Shader::SwStage::Vertex)], *fragment,
            runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs,
            std::min(limits.maxGeometryOutputComponents, limits.maxFragmentInputComponents),
            limits.maxGeometryTotalOutputComponents);
        LOG_INFO(Render_Vulkan, "Software interpolation GS: vs={:#x} fs={:#x} words={}",
                 vertex->pgm_hash, fragment->pgm_hash, sdata.interpolation_gs.size());
    }
    if (!sdata.interpolation_gs.empty()) {
        software_interpolation = true;
        s.shader_stages.emplace_back(
            vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eGeometry,
                                              .module = aux_module(sdata.interpolation_gs),
                                              .pName = "main"});
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto locations = Shader::Backend::SPIRV::AuxiliaryVaryingLocations(
                *infos[u32(Shader::SwStage::Vertex)], profile);
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, locations,
                key.emulate_depth_range && (runtime_infos[u32(Shader::SwStage::Vertex)].depth_range.clip_near ||
                                           runtime_infos[u32(Shader::SwStage::Vertex)].depth_range.clip_far),
                Shader::Backend::SPIRV::AuxiliaryBuiltinLocations(
                    *infos[u32(Shader::SwStage::Vertex)], profile));
        }
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = aux_module(sdata.tcs),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            const auto locations = Shader::Backend::SPIRV::AuxiliaryVaryingLocations(
                *infos[u32(Shader::SwStage::Vertex)], profile);
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, locations,
                key.emulate_depth_range && (runtime_infos[u32(Shader::SwStage::Vertex)].depth_range.clip_near ||
                                           runtime_infos[u32(Shader::SwStage::Vertex)].depth_range.clip_far),
                Shader::Backend::SPIRV::AuxiliaryBuiltinLocations(
                    *infos[u32(Shader::SwStage::Vertex)], profile));
        }
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = aux_module(sdata.tes),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        if (!preloading) {
            const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

            sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        }
        s.shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = aux_module(sdata.fragment),
            .pName = "main",
        });
    }

    // A PS4 wave has 64 lanes. U64 lane masks (ballot/inverse ballot) and reductions ending in
    // reads of lanes 31/63 must not combine halves of a 128-lane host wave (Turnip's default
    // fragment size), so request 64 wherever the device allows it for that stage.
    s.subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    for (auto& stage_ci : s.shader_stages) {
        stage_ci.pNext =
            instance.IsSubgroupSize64Supported(stage_ci.stage) ? &s.subgroup_size_ci : nullptr;
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        s.color_formats[i] = color_format;
    }

    const auto validate_samples = [&](vk::Format format, u32 samples, bool depth) {
        const auto supported = instance.GetAttachmentSampleCounts(format, depth);
        ASSERT_MSG(samples && std::has_single_bit(samples) && (u32(supported) & samples),
                   "Unsupported attachment samples: format={} depth={} requested={} supported={:#x}",
                   vk::to_string(format), depth, samples, u32(supported));
        return static_cast<vk::SampleCountFlagBits>(samples);
    };
    for (u32 i = 0; i < key.num_color_attachments; ++i) {
        s.color_samples[i] = key.color_samples[i] && s.color_formats[i] != vk::Format::eUndefined
            ? validate_samples(s.color_formats[i], key.color_samples[i], false)
            : vk::SampleCountFlagBits::e1;
    }
    const bool has_depth = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid ||
                           key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid;
    s.mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = s.color_samples.data(),
        .depthStencilAttachmentSamples = has_depth
            ? validate_samples(depth_format, key.depth_samples, true)
            : vk::SampleCountFlagBits::e1,
    };
    if (!key.num_color_attachments && !has_depth) {
        ASSERT_MSG(u32(instance.GetPhysicalDevice().getProperties().limits.framebufferNoAttachmentsSampleCounts) &
                       key.num_samples, "Unsupported attachment-less raster sample count {}", key.num_samples);
    }

    s.pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &s.mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = s.color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        s.attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
            s.attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            s.attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            s.attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            s.attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    s.color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = s.attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    s.depth_stencil_info = {};

    s.probe.emplace(static_cast<u32>(s.shader_stages.size()));
    s.pipeline_info = {
        .pNext = s.probe->Chain(&s.pipeline_rendering_ci),
        .stageCount = static_cast<u32>(s.shader_stages.size()),
        .pStages = s.shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &s.vertex_input_info : nullptr,
        .pInputAssemblyState = &s.input_assembly,
        .pTessellationState = &s.tessellation_state,
        .pViewportState = &s.viewport_info,
        .pRasterizationState = &s.raster_chain.get(),
        .pMultisampleState = &s.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &s.depth_stencil_info : nullptr,
        .pColorBlendState = &s.color_blending,
        .pDynamicState = &s.dynamic_info,
        .layout = *pipeline_layout,
    };
    s.fs_runtime = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;

    if (defer_build) {
        MarkPending();
    } else {
        CreateNative();
    }
}

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::CreateNative() const {
    auto& s = *create_state;
    const vk::Device device = instance.GetDevice();
    {
        const auto serialize = SerializePipelineCreate(instance);
        s.probe->Start();
        auto [pipeline_result, pipe] = [&] {
            Common::Profiler::Scope scope{"Pipeline.CreateGraphics"};
            return device.createGraphicsPipelineUnique(s.pipeline_cache, s.pipeline_info);
        }();
        creation = s.probe->Finish();
        if (pipeline_result != vk::Result::eSuccess) {
            // Failure-only diagnostics: no per-draw file I/O or extra synchronization.
            nlohmann::json data={{"pipeline",s.debug_str},{"result",vk::to_string(pipeline_result)}};
            data["stages"]=nlohmann::json::array();
            for (u32 stage=0;stage<MaxShaderStages;++stage) {
                if (!stages[stage]) continue;
                nlohmann::json params=nlohmann::json::array();
                for (u32 i=0;i<Shader::IR::NumParams;++i) {
                    const auto attr=Shader::IR::Attribute::Param0+i;
                    params.push_back({{"param",i},{"load",stages[stage]->loads.GetAny(attr)},
                                      {"store",stages[stage]->stores.GetAny(attr)}});
                }
                data["stages"].push_back({{"stage",stage},{"hash",stages[stage]->pgm_hash},
                    {"key",key.stage_hashes[stage]},{"params",std::move(params)}});
            }
            const auto& fs=s.fs_runtime;
            data["fs_inputs"]=nlohmann::json::array();
            for (u32 i=0;i<fs.num_inputs;++i) data["fs_inputs"].push_back({{"param",i},
                {"location",fs.inputs[i].param_index},{"default",fs.inputs[i].IsDefault()}});
            data["clip_distance_emulation"]=fs.clip_distance_emulation;
            std::ofstream out(Common::FS::GetUserPath(Common::FS::PathType::LogDir)/"failed-graphics-pipeline.json");
            out<<data.dump(2);out.close();
        }
        ASSERT_MSG(pipeline_result == vk::Result::eSuccess,
                   "Failed to create graphics pipeline: {}", vk::to_string(pipeline_result));
        pipeline = std::move(pipe);
    }
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", s.debug_str);
    create_state.reset(); // Also destroys the host-generated stage modules.
}

void GraphicsPipeline::ApplyFragmentShadingRate(const RecordingCommandBuffer& cmd, u32 quality) const {
    if (!instance.IsPipelineFragmentShadingRateSupported()) {
        return;
    }
    const auto size = instance.GetFragmentShadingRates().Select(
        quality, raster_samples, requires_full_fragment_rate);
    cmd.Custom(0, [size](vk::CommandBuffer c) { SetGuestFragmentShadingRate(c, size); });
    // Bounded diagnostics: one record per requested/effective pair per render
    // thread, not one per draw or pipeline. Unsupported qualities use full rate.
    static thread_local u32 reported{};
    const u32 effective = size.width == 1 ? 2 : (size.height == 1 ? 1 : 0);
    const u32 bit = 1U << (std::min(quality, 2U) * 3 + effective);
    if (!(reported & bit)) {
        reported |= bit;
        LOG_INFO(Render_Vulkan,
                 "Guest shading rate: quality={} effective={}x{} samples={} full_rate_required={} FDM=off",
                 quality, size.width, size.height, static_cast<u32>(raster_samples),
                 requires_full_fragment_rate);
    }
}


template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage); // See for the comment in compute PL creation
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
