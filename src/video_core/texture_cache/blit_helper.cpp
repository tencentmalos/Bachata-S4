// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include "spatial/texture_codec/AstcEncoder.h"
#include "spatial/texture_codec/Bc7Encoder.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/vma_diagnostics.h"

#include "video_core/host_shaders/color_to_ms_depth_frag.h"
#include "video_core/host_shaders/depth_resample_frag.h"
#include "video_core/host_shaders/depth_stencil_resample_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/ms_image_blit_frag.h"
#include "video_core/host_shaders/ms_image_blit_msaa_frag.h"

namespace VideoCore {

static vk::SampleCountFlagBits ToSampleCount(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return vk::SampleCountFlagBits::e1;
    case 2:
        return vk::SampleCountFlagBits::e2;
    case 4:
        return vk::SampleCountFlagBits::e4;
    case 8:
        return vk::SampleCountFlagBits::e8;
    case 16:
        return vk::SampleCountFlagBits::e16;
    default:
        UNREACHABLE_MSG("Unknown samples count = {}", num_samples);
    }
}

BlitHelper::BlitHelper(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    CreateShaders();
    CreatePipelineLayouts();
}

BlitHelper::~BlitHelper() {
    const auto device = instance.GetDevice();
    device.destroy(fs_tri_vertex);
    device.destroy(color_to_ms_depth_frag);
    device.destroy(src_msaa_copy_frag);
    device.destroy(src_non_msaa_copy_frag);
    device.destroy(depth_resample_frag);
    device.destroy(depth_stencil_resample_frag);
}

bool BlitHelper::CanResampleDepth(vk::Format format, bool stencil) const {
    return (!stencil || depth_stencil_resample_frag) &&
           instance.IsFormatSupported(format, vk::FormatFeatureFlagBits2::eSampledImage |
                                                  vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
}

void BlitHelper::ResampleDepthStencil(vk::Image source, u32 source_mip, vk::Image dest,
                                      u32 dest_mip, u32 layer, vk::Format format,
                                      vk::ImageAspectFlags aspects, u32 dest_width,
                                      u32 dest_height, bool write_stencil) {
    ASSERT(CanResampleDepth(format, write_stencil));
    const auto device = instance.GetDevice();
    const auto make_view = [&](vk::Image image, u32 mip, vk::ImageAspectFlags view_aspects,
                               vk::ImageUsageFlags usage) {
        const vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
        const auto [result, view] = device.createImageView({
            .pNext = &usage_ci,
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {view_aspects, mip, 1, layer, 1},
        });
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create depth resample view: {}",
                   vk::to_string(result));
        return view;
    };
    // A sampled view selects one aspect; the attachment view covers the image's aspects.
    const auto depth_view =
        make_view(source, source_mip, vk::ImageAspectFlagBits::eDepth, vk::ImageUsageFlagBits::eSampled);
    const auto stencil_view =
        write_stencil ? make_view(source, source_mip, vk::ImageAspectFlagBits::eStencil,
                                  vk::ImageUsageFlagBits::eSampled)
                      : vk::ImageView{};
    const auto target_view =
        make_view(dest, dest_mip, aspects, vk::ImageUsageFlagBits::eDepthStencilAttachment);
    scheduler.DeferOperation([device, depth_view, stencil_view, target_view] {
        device.destroyImageView(depth_view);
        if (stencil_view) {
            device.destroyImageView(stencil_view);
        }
        device.destroyImageView(target_view);
    });

    // Every pixel is written, so nothing is loaded.
    Vulkan::RenderState state{};
    state.width = static_cast<u16>(dest_width);
    state.height = static_cast<u16>(dest_height);
    state.num_layers = 1;
    auto& target = state.depth_stencil_attachment;
    target.image_view = target_view;
    target.image_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    target.has_depth = true;
    target.depth_clear = true;
    target.has_stencil = write_stencil;
    target.stencil_clear = write_stencil;
    scheduler.BeginRendering(state);
    scheduler.UntrackPass(); // its accesses are not staged

    const vk::DescriptorImageInfo depth_info{
        .imageView = depth_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo stencil_info{
        .imageView = stencil_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const std::array writes{
        vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eSampledImage,
                               .pImageInfo = &depth_info},
        vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eSampledImage,
                               .pImageInfo = &stencil_info},
    };
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eGraphics, *depth_resample_pl_layout,
                                  *depth_resample_descriptor_set_layout,
                                  vk::ArrayProxy<const vk::WriteDescriptorSet>(
                                      write_stencil ? 2u : 1u, writes.data()));

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                        DepthResamplePipeline(format, write_stencil));
    const std::array<s32, 2> extent{static_cast<s32>(dest_width), static_cast<s32>(dest_height)};
    cmdbuf.pushConstants(*depth_resample_pl_layout, vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(extent), extent.data());
    const vk::Viewport viewport{
        .width = float(dest_width),
        .height = float(dest_height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);
    cmdbuf.setScissorWithCount(vk::Rect2D{.extent = {dest_width, dest_height}});
    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering(Vulkan::RenderBreak::ImageCopy);
    scheduler.GetDynamicState().Invalidate();
}

vk::Pipeline BlitHelper::DepthResamplePipeline(vk::Format format, bool stencil) {
    const DepthResampleKey key{format, stencil};
    if (const auto it = std::ranges::find(depth_resample_pl, key,
                                          &decltype(depth_resample_pl)::value_type::first);
        it != depth_resample_pl.end()) {
        return *it->second;
    }
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    // The shader exports the reference; replace it on every outcome.
    const vk::StencilOpState stencil_op{
        .failOp = vk::StencilOp::eReplace,
        .passOp = vk::StencilOp::eReplace,
        .depthFailOp = vk::StencilOp::eReplace,
        .compareOp = vk::CompareOp::eAlways,
        .compareMask = 0xFF,
        .writeMask = 0xFF,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = true,
        .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
        .stencilTestEnable = stencil,
        .front = stencil_op,
        .back = stencil_op,
    };
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount,
                                    vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const std::array shader_stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = fs_tri_vertex,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = stencil ? depth_stencil_resample_frag : depth_resample_frag,
            .pName = "main",
        },
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .depthAttachmentFormat = format,
        .stencilAttachmentFormat = stencil ? format : vk::Format::eUndefined,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *depth_resample_pl_layout,
    };
    auto [result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create depth resample pipeline: {}",
               vk::to_string(result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Depth resample {}{}",
                          vk::to_string(format), stencil ? " +stencil" : "");
    return *depth_resample_pl.emplace_back(key, std::move(pipeline)).second;
}

void BlitHelper::EncodeBlocks(BlockCodec codec, vk::Image source, vk::Format source_format, u32 source_mip,
                              vk::Image dest, u32 dest_mip, u32 width, u32 height, u32 layers,
                              bool srgb, u32 block_dim) {
    using namespace spatial::texture_codec;
    ASSERT(codec != BlockCodec::None && (codec == BlockCodec::Astc || block_dim == 4));
    auto& slot = codec == BlockCodec::Bc7 ? bc7_encoder : astc_encoder;
    if (!slot) {
        auto encoder = std::make_unique<VulkanAstcEncoder>();
        const auto shader = Vulkan::Compile(codec == BlockCodec::Bc7 ? Bc7Shader() : AstcLdrShader(),
                                            vk::ShaderStageFlagBits::eCompute, instance.GetDevice());
        const auto result = encoder->Initialize(instance.GetDevice(),
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, shader,
            bool(instance.HostDescriptorFlags() & vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR));
        instance.GetDevice().destroyShaderModule(shader);
        ASSERT_MSG(result == VK_SUCCESS, "{} encoder initialization failed: {}", BlockCodecName(codec), int(result));
        slot = std::move(encoder);
    }
    const auto& encoder = *slot;
    const AstcRequest request{width, height, layers, u32(srgb), block_dim};
    ASSERT(request.Valid());
    // Each dispatch's blocks are copied into the destination immediately. Reuse
    // one ordered scratch allocation instead of retaining one per texture/mip
    // until the loading frame is submitted.
    if (!block_encode_scratch || block_encode_scratch->SizeBytes() < request.Bytes()) {
        const u64 limit = instance.MaxBufferSize() ? instance.MaxBufferSize()
                                                   : instance.MaxMemoryAllocationSize();
        if (!request.Bytes() || request.Bytes() > limit) {
            throw std::runtime_error("Block encode scratch exceeds device buffer size limit");
        }
        const u64 capacity =
            std::min<u64>(std::bit_ceil(std::max<u64>(request.Bytes(), 16384)), limit);
        auto replacement = std::make_unique<Buffer>(instance, 0, capacity, MemoryType::DeviceLocal,
                                                    "BlitHelper:BlockEncode");
        VmaDiagnostics::Tag(instance.GetAllocator(), replacement->buffer.allocation,
                            "scratch/block-encode");
        if (block_encode_scratch) {
            VmaDiagnostics::Tag(instance.GetAllocator(), block_encode_scratch->buffer.allocation,
                                nullptr, true);
            scheduler.DeferOperation([retired = std::move(block_encode_scratch)] {});
        }
        block_encode_scratch = std::move(replacement);
    }
    const auto* blocks = block_encode_scratch.get();
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
    const auto [result, view] = instance.GetDevice().createImageView({
        .pNext = &usage, .image = source, .viewType = vk::ImageViewType::e2DArray,
        .format = source_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, source_mip, 1, 0, layers},
    });
    ASSERT(result == vk::Result::eSuccess);
    const vk::DescriptorImageInfo input{encoder.Sampler(), view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorBufferInfo output{blocks->Handle(), 0, request.Bytes()};
    const std::array writes{
        vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &input},
        vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &output},
    };
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eCompute, encoder.PipelineLayout(),
                                   encoder.DescriptorLayout(), writes);
    const auto cmd = scheduler.RawCommandBuffer();
    const vk::BufferMemoryBarrier2 reuse{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = blocks->Handle(),
        .size = request.Bytes(),
    };
    cmd.pipelineBarrier2(
        vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &reuse});
    ASSERT(encoder.Record(cmd, VK_NULL_HANDLE, request));
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = blocks->Handle(),
        .size = request.Bytes(),
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
    const vk::BufferImageCopy copy{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, dest_mip, 0, layers},
        .imageExtent = {width, height, 1},
    };
    cmd.copyBufferToImage(blocks->Handle(), dest, vk::ImageLayout::eTransferDstOptimal, copy);
    scheduler.DeferOperation(
        [device = instance.GetDevice(), view] { device.destroyImageView(view); });
}

void BlitHelper::ReinterpretColorAsMsDepth(u32 width, u32 height, u32 num_samples,
                                           vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                           vk::Image source, vk::Image dest) {
    const vk::ImageViewUsageCreateInfo color_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo color_view_ci = {
        .pNext = &color_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = src_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [color_view_result, color_view] =
        instance.GetDevice().createImageView(color_view_ci);
    ASSERT_MSG(color_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(color_view_result));
    const vk::ImageViewUsageCreateInfo depth_usage_ci{
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment};
    const vk::ImageViewCreateInfo depth_view_ci = {
        .pNext = &depth_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = dst_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [depth_view_result, depth_view] =
        instance.GetDevice().createImageView(depth_view_ci);
    ASSERT_MSG(depth_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(depth_view_result));
    scheduler.DeferOperation([device = instance.GetDevice(), color_view, depth_view] {
        device.destroyImageView(color_view);
        device.destroyImageView(depth_view);
    });

    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.depth_stencil_attachment.image_view = depth_view;
    state.depth_stencil_attachment.image_layout = vk::ImageLayout::eDepthAttachmentOptimal;
    state.depth_stencil_attachment.has_depth = true;
    state.depth_stencil_attachment.depth_clear = true;
    scheduler.BeginRendering(state);
    scheduler.UntrackPass(); // its accesses are not staged

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = color_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, *single_texture_descriptor_set_layout, texture_write);

    const MsPipelineKey key{num_samples, dst_pixel_format, false};
    auto it = std::ranges::find(color_to_ms_depth_pl, key, &MsPipeline::first);
    if (it == color_to_ms_depth_pl.end()) {
        CreateColorToMSDepthPipeline(key);
        it = --color_to_ms_depth_pl.end();
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *it->second);

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering(Vulkan::RenderBreak::ImageCopy);
    scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::CopyBetweenMsImages(u32 width, u32 height, u32 num_samples,
                                     vk::Format pixel_format, bool src_msaa, vk::Image source,
                                     vk::Image dest) {
    const vk::ImageViewUsageCreateInfo src_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo src_view_ci = {
        .pNext = &src_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [src_view_result, src_view] = instance.GetDevice().createImageView(src_view_ci);
    ASSERT_MSG(src_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(src_view_result));

    const vk::ImageViewUsageCreateInfo dst_usage_ci{.usage =
                                                        vk::ImageUsageFlagBits::eColorAttachment};
    const vk::ImageViewCreateInfo dst_view_ci = {
        .pNext = &dst_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [dst_view_result, dst_view] = instance.GetDevice().createImageView(dst_view_ci);
    ASSERT_MSG(dst_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(dst_view_result));
    scheduler.DeferOperation([device = instance.GetDevice(), src_view, dst_view] {
        device.destroyImageView(src_view);
        device.destroyImageView(dst_view);
    });

    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.num_color_attachments = 1;
    state.color_attachments[0].image_view = dst_view;
    state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    state.color_attachments[0].is_clear = true;
    scheduler.BeginRendering(state);
    scheduler.UntrackPass(); // its accesses are not staged

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = src_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, *single_texture_descriptor_set_layout, texture_write);

    const MsPipelineKey key{num_samples, pixel_format, src_msaa};
    auto it = std::ranges::find(ms_image_copy_pl, key, &MsPipeline::first);
    if (it == ms_image_copy_pl.end()) {
        CreateMsCopyPipeline(key);
        it = --ms_image_copy_pl.end();
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *it->second);

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering(Vulkan::RenderBreak::ImageCopy);
    scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::CreateShaders() {
    const auto device = instance.GetDevice();
    fs_tri_vertex = Vulkan::CompileSPV(FS_TRI_VERT, device);
    color_to_ms_depth_frag = Vulkan::CompileSPV(COLOR_TO_MS_DEPTH_FRAG, device);
    src_msaa_copy_frag = Vulkan::CompileSPV(MS_IMAGE_BLIT_MSAA_FRAG, device);
    src_non_msaa_copy_frag = Vulkan::CompileSPV(MS_IMAGE_BLIT_FRAG, device);
    depth_resample_frag = Vulkan::CompileSPV(DEPTH_RESAMPLE_FRAG, device);
    // The stencil variant declares StencilExportEXT, valid only with the extension enabled.
    if (instance.IsShaderStencilExportSupported()) {
        depth_stencil_resample_frag = Vulkan::CompileSPV(DEPTH_STENCIL_RESAMPLE_FRAG, device);
    }
}

void BlitHelper::CreatePipelineLayouts() {
    const vk::DescriptorSetLayoutBinding texture_binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = instance.HostDescriptorFlags(),
        .bindingCount = 1U,
        .pBindings = &texture_binding,
    };
    auto [desc_layout_result, desc_layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    single_texture_descriptor_set_layout = std::move(desc_layout);
    const vk::DescriptorSetLayout set_layout = *single_texture_descriptor_set_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, pipeline_layout] =
        instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline_layout, "Single texture pipeline layout");
    single_texture_pl_layout = std::move(pipeline_layout);

    // Depth resample: depth and stencil planes as two sampled images, target extent pushed.
    const std::array resample_bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };
    auto [resample_set_result, resample_set_layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique({
            .flags = instance.HostDescriptorFlags(),
            .bindingCount = static_cast<u32>(resample_bindings.size()),
            .pBindings = resample_bindings.data(),
        });
    ASSERT_MSG(resample_set_result == vk::Result::eSuccess,
               "Failed to create depth resample descriptor set layout: {}",
               vk::to_string(resample_set_result));
    depth_resample_descriptor_set_layout = std::move(resample_set_layout);
    const vk::DescriptorSetLayout resample_layout = *depth_resample_descriptor_set_layout;
    const vk::PushConstantRange extent_range{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = 2 * sizeof(s32),
    };
    auto [resample_result, resample_pl_layout] =
        instance.GetDevice().createPipelineLayoutUnique({
            .setLayoutCount = 1U,
            .pSetLayouts = &resample_layout,
            .pushConstantRangeCount = 1U,
            .pPushConstantRanges = &extent_range,
        });
    ASSERT_MSG(resample_result == vk::Result::eSuccess,
               "Failed to create depth resample pipeline layout: {}",
               vk::to_string(resample_result));
    Vulkan::SetObjectName(instance.GetDevice(), *resample_pl_layout,
                          "Depth resample pipeline layout");
    depth_resample_pl_layout = std::move(resample_pl_layout);
}

void BlitHelper::CreateColorToMSDepthPipeline(const MsPipelineKey& key) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = ToSampleCount(key.num_samples),
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = true,
        .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = color_to_ms_depth_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 0U,
        .pColorAttachmentFormats = nullptr,
        .depthAttachmentFormat = key.attachment_format,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *single_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Color to MS Depth {}", key.num_samples);

    color_to_ms_depth_pl.emplace_back(key, std::move(pipeline));
}

void BlitHelper::CreateMsCopyPipeline(const MsPipelineKey& key) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = ToSampleCount(key.num_samples),
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = key.src_msaa ? src_msaa_copy_frag : src_non_msaa_copy_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 1u,
        .pColorAttachmentFormats = &key.attachment_format,
        .depthAttachmentFormat = vk::Format::eUndefined,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendAttachmentState attachment = {
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1u,
        .pAttachments = &attachment,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *single_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Non MS Image to MS Image {}",
                          key.num_samples);

    ms_image_copy_pl.emplace_back(key, std::move(pipeline));
}

} // namespace VideoCore
