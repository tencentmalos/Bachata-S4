// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <tsl/robin_map.h>
#include <memory>
namespace spatial::texture_codec { class VulkanAstcEncoder; }

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/scale_policy.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

class Image;
class Buffer;
class ImageView;
struct ImageInfo;

class BlitHelper {
    static constexpr size_t MaxMsPipelines = 6;

public:
    explicit BlitHelper(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~BlitHelper();

    void ReinterpretColorAsMsDepth(u32 width, u32 height, u32 num_samples,
                                   vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                   vk::Image source, vk::Image dest);

    // Records sample/resample -> ASTC or BC7 blocks -> destination mip on the existing queue.
    // block_dim selects ASTC 4x4 or 6x6; BC7 blocks are always 4x4.
    void EncodeBlocks(BlockCodec codec, vk::Image source, vk::Format source_format, u32 source_mip,
                      vk::Image dest, u32 dest_mip, u32 width, u32 height, u32 layers, bool srgb,
                      u32 block_dim = 4);

    void CopyBetweenMsImages(u32 width, u32 height, u32 num_samples, vk::Format pixel_format,
                             bool src_msaa, vk::Image source, vk::Image dest);

    // Whether ResampleDepthStencil can copy this format; stencil needs stencil export.
    bool CanResampleDepth(vk::Format format, bool stencil) const;

    // Nearest copy of one depth (and optionally stencil) subresource into another extent, as
    // a nearest blit would, for depth formats the driver cannot blit into. `aspects` are the
    // image's aspects. The source must be in ShaderReadOnlyOptimal and the destination in
    // DepthStencilAttachmentOptimal; without `write_stencil` its stencil plane is kept.
    void ResampleDepthStencil(vk::Image source, u32 source_mip, vk::Image dest, u32 dest_mip,
                              u32 layer, vk::Format format, vk::ImageAspectFlags aspects,
                              u32 dest_width, u32 dest_height, bool write_stencil);

private:
    void CreateShaders();
    void CreatePipelineLayouts();
    vk::Pipeline DepthResamplePipeline(vk::Format format, bool stencil);

    struct MsPipelineKey {
        u32 num_samples;
        vk::Format attachment_format;
        bool src_msaa;

        auto operator<=>(const MsPipelineKey&) const noexcept = default;
    };
    void CreateColorToMSDepthPipeline(const MsPipelineKey& key);
    void CreateMsCopyPipeline(const MsPipelineKey& key);

private:
    // Both run on the codec module's encoder class; they differ only in their shader.
    std::unique_ptr<spatial::texture_codec::VulkanAstcEncoder> astc_encoder;
    std::unique_ptr<spatial::texture_codec::VulkanAstcEncoder> bc7_encoder;
    std::unique_ptr<Buffer> block_encode_scratch;
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    vk::UniqueDescriptorSetLayout single_texture_descriptor_set_layout;
    vk::UniquePipelineLayout single_texture_pl_layout;
    vk::ShaderModule fs_tri_vertex;
    vk::ShaderModule color_to_ms_depth_frag;
    vk::ShaderModule src_msaa_copy_frag;
    vk::ShaderModule src_non_msaa_copy_frag;
    vk::ShaderModule depth_resample_frag;
    vk::ShaderModule depth_stencil_resample_frag; // Only with shader stencil export.
    vk::UniqueDescriptorSetLayout depth_resample_descriptor_set_layout;
    vk::UniquePipelineLayout depth_resample_pl_layout;

    using MsPipeline = std::pair<MsPipelineKey, vk::UniquePipeline>;
    std::vector<MsPipeline> color_to_ms_depth_pl;
    std::vector<MsPipeline> ms_image_copy_pl;
    struct DepthResampleKey {
        vk::Format format;
        bool stencil;
        bool operator==(const DepthResampleKey&) const noexcept = default;
    };
    std::vector<std::pair<DepthResampleKey, vk::UniquePipeline>> depth_resample_pl;
};

} // namespace VideoCore
