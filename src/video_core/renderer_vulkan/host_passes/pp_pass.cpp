//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/pp_pass.h"

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/post_process_frag.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <boost/container/static_vector.hpp>

namespace Vulkan::HostPasses {

void PostProcessingPass::Create(const Instance& instance, const vk::Format surface_format) {
    device = instance.GetDevice();
    fdm_dynamic = instance.IsFdmDynamicSupported();
    static const std::array pp_shaders{
        HostShaders::FS_TRI_VERT,
        HostShaders::POST_PROCESS_FRAG,
    };

    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) bindings[i] = {
        .binding = i, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci{
        .flags = instance.HostDescriptorFlags(),
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };

    desc_set_layout = Check<"create pp descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Settings),
    };

    const auto& vs_module = Compile(pp_shaders[0], vk::ShaderStageFlagBits::eVertex, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");

    const auto& fs_module = Compile(pp_shaders[1], vk::ShaderStageFlagBits::eFragment, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "post_process.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };

    pipeline_layout =
        Check<"create pp pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const std::array pp_color_formats{
        surface_format,
    };
    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci{
        .colorAttachmentCount = pp_color_formats.size(),
        .pColorAttachmentFormats = pp_color_formats.data(),
    };

    // Turnip currently exposes VK_EXT_fragment_density_map but may leave
    // fragmentDensityMapDynamic disabled. Build the traditional render-pass
    // variant once so the same map still works without a second image/blit.
    if (instance.IsFdmSupported() && !fdm_dynamic) {
        const auto fdm_attachment = spatial::foveation::vulkan::MakeRenderPassFdmAttachment(1);
        const std::array attachment_descriptions{
            vk::AttachmentDescription2{
                .format = surface_format,
                .samples = vk::SampleCountFlagBits::e1,
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
            },
            fdm_attachment.description,
        };
        const vk::AttachmentReference2 color_reference{
            .attachment = 0,
            .layout = vk::ImageLayout::eColorAttachmentOptimal,
        };
        const vk::SubpassDescription2 subpass{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_reference,
        };
        const auto dependency = spatial::foveation::vulkan::MakeFdmExternalDependency();
        const vk::RenderPassCreateInfo2 render_pass_info{
            .pNext = &fdm_attachment.create_info,
            .attachmentCount = attachment_descriptions.size(),
            .pAttachments = attachment_descriptions.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 1,
            .pDependencies = &dependency,
        };
        fdm_render_pass = Check<"create FDM post process render pass">
            (device.createRenderPass2Unique(render_pass_info));
    }

    const vk::PipelineCreateFlags pipeline_flags = fdm_dynamic
        ? vk::PipelineCreateFlagBits::eRenderingFragmentDensityMapAttachmentEXT
        : vk::PipelineCreateFlags{};

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{
        .vertexBindingDescriptionCount = 0u,
        .vertexAttributeDescriptionCount = 0u,
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    const std::array attachments{
        vk::PipelineColorBlendAttachmentState{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        },
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = attachments.size(),
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .flags = pipeline_flags,
        .pNext = &pipeline_rendering_ci,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    pipeline = Check<"create post process pipeline">(device.createGraphicsPipelineUnique(
        /*pipeline_cache*/ {}, pipeline_info));

    if (fdm_render_pass) {
        auto fdm_pipeline_info = pipeline_info;
        // The legacy render-pass pipeline does not use the dynamic-rendering
        // fragment-density pipeline flag. The attachment is declared by the
        // render-pass pNext chain above instead.
        fdm_pipeline_info.flags = {};
        fdm_pipeline_info.pNext = nullptr;
        fdm_pipeline_info.renderPass = *fdm_render_pass;
        fdm_pipeline = Check<"create FDM post process pipeline">(
            device.createGraphicsPipelineUnique(/*pipeline_cache*/ {}, fdm_pipeline_info));
    }

    // Once pipeline is compiled, we don't need the shader module anymore
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    // Create sampler resource
    const vk::SamplerCreateInfo sampler_ci{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create pp sampler">(device.createSamplerUnique(sampler_ci));
}

void PostProcessingPass::Render(Scheduler& scheduler, vk::ImageView input,
                                vk::Extent2D input_size, Frame& frame, Settings settings,
                                std::array<vk::ImageView, 3> stereo_views,
                                vk::ImageView fdm_view,
                                std::array<vk::Sampler, 4> stereo_samplers) {
    const auto cmdbuf = scheduler.CommandBuffer();
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/Post processing",
        });
    }

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array<vk::RenderingAttachmentInfo, 1> attachments{{
        {
            .imageView = frame.image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    }};
    const bool use_render_pass_fdm = fdm_view && fdm_render_pass && fdm_pipeline;

    std::array<vk::DescriptorImageInfo, 4> image_infos{};
    std::array<vk::WriteDescriptorSet, 4> set_writes{};
    for (u32 i = 0; i < image_infos.size(); ++i) {
        image_infos[i] = {.sampler = stereo_samplers[i] ? stereo_samplers[i] : *sampler,
            .imageView = i && stereo_views[i - 1] ? stereo_views[i - 1] : input,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
        set_writes[i] = {.dstSet = VK_NULL_HANDLE, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[i]};
    }

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                        use_render_pass_fdm ? *fdm_pipeline : *pipeline);

    const std::array viewports = {
        vk::Viewport{
            .width = static_cast<float>(frame.width),
            .height = static_cast<float>(frame.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        },
    };

    cmdbuf.setViewport(0, viewports);
    cmdbuf.setScissor(0, vk::Rect2D{
                             .extent{
                                 .width = frame.width,
                                 .height = frame.height,
                             },
                         });

    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eGraphics, *pipeline_layout, *desc_set_layout, set_writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(Settings),
                         &settings);

    if (use_render_pass_fdm) {
        const std::array framebuffer_attachments{frame.image_view, fdm_view};
        const vk::FramebufferCreateInfo framebuffer_info{
            .renderPass = *fdm_render_pass,
            .attachmentCount = framebuffer_attachments.size(),
            .pAttachments = framebuffer_attachments.data(),
            .width = frame.width,
            .height = frame.height,
            .layers = 1,
        };
        const auto framebuffer = Check<"create FDM post process framebuffer">
            (device.createFramebuffer(framebuffer_info));
        const auto device_handle = device;
        scheduler.DeferOperation([device_handle, framebuffer] {
            device_handle.destroyFramebuffer(framebuffer);
        });
        const vk::ClearValue clear_value{
            .color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}}};
        const vk::RenderPassBeginInfo begin_info{
            .renderPass = *fdm_render_pass,
            .framebuffer = framebuffer,
            .renderArea = {.extent = {frame.width, frame.height}},
            .clearValueCount = 1,
            .pClearValues = &clear_value,
        };
        cmdbuf.beginRenderPass(begin_info, vk::SubpassContents::eInline);
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRenderPass();
    } else {
        const vk::RenderingFragmentDensityMapAttachmentInfoEXT fdm_attachment{
            .imageView = fdm_view,
            .imageLayout = vk::ImageLayout::eFragmentDensityMapOptimalEXT,
        };
        const vk::RenderingInfo rendering_info{
            .pNext = (fdm_view && fdm_dynamic) ? &fdm_attachment : nullptr,
            .renderArea{.extent{.width = frame.width, .height = frame.height}},
            .layerCount = 1,
            .colorAttachmentCount = attachments.size(),
            .pColorAttachments = attachments.data(),
        };
        cmdbuf.beginRendering(rendering_info);
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRendering();
    }

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = simple_subresource,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

} // namespace Vulkan::HostPasses
