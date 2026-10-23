//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/present_pass.h"

#include "common/assert.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/present_frame_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

void PresentPass::Create(const Instance& instance) {
    device = instance.GetDevice();

    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    desc_set_layout = Check<"create present descriptor set layout">(
        device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
            .flags = instance.HostDescriptorFlags(),
            .bindingCount = 1,
            .pBindings = &binding,
        }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = Check<"create present pipeline layout">(
        device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
            .setLayoutCount = 1,
            .pSetLayouts = &*desc_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));

    vs_module = vk::UniqueShaderModule{CompileSPV(FS_TRI_VERT, device), device};
    ASSERT(*vs_module);
    SetObjectName(device, *vs_module, "fs_tri.vert");
    fs_module = vk::UniqueShaderModule{CompileSPV(PRESENT_FRAME_FRAG, device), device};
    ASSERT(*fs_module);
    SetObjectName(device, *fs_module, "present_frame.frag");

    // Bilinear scaling to the rectangle, as the post-process pass did. At equal sizes it
    // returns the texels unchanged.
    sampler = Check<"create present sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));
}

vk::Pipeline PresentPass::PipelineFor(vk::Format format) {
    for (const auto& [cached, pipeline] : pipelines) {
        if (cached == format) {
            return *pipeline;
        }
    }
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs_module, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };
    auto pipeline = Check<"create present pipeline">(device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
                .pNext = &rendering_ci,
                .stageCount = stages.size(),
                .pStages = stages.data(),
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_info,
                .pRasterizationState = &raster_state,
                .pMultisampleState = &multisampling,
                .pColorBlendState = &color_blending,
                .pDynamicState = &dynamic_info,
                .layout = *pipeline_layout,
            }));
    const vk::Pipeline handle = *pipeline;
    pipelines.emplace_back(format, std::move(pipeline));
    return handle;
}

void PresentPass::Draw(Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::Format target_format,
                       const DrawInfo& info) {
    const auto& rect = info.rect;
    if (!info.picture || rect.extent.width == 0 || rect.extent.height == 0) {
        return;
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, PipelineFor(target_format));
    cmdbuf.setViewport(0, vk::Viewport{
                              .x = static_cast<float>(rect.offset.x),
                              .y = static_cast<float>(rect.offset.y),
                              .width = static_cast<float>(rect.extent.width),
                              .height = static_cast<float>(rect.extent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f,
                          });
    cmdbuf.setScissor(0, rect);
    const vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = info.picture,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_info,
    };
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eGraphics, *pipeline_layout,
                                  *desc_set_layout, write);
    const PushConstants push{
        .rotation = info.rotation,
        .encode = info.encode ? 1u : 0u,
        .srgb_input = info.srgb_input ? 1u : 0u,
        .gamma = info.gamma,
    };
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push),
                         &push);
    cmdbuf.draw(3, 1, 0, 0);
}

} // namespace Vulkan::HostPasses
