// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>
#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"

namespace VideoCore {

struct ImageInfo;
struct Image;
class StreamBuffer;

class TileManager {
    static constexpr size_t NUM_BPPS = 5;

public:
    using ScratchBuffer = std::pair<vk::Buffer, VmaAllocation>;
    using Result = std::pair<vk::Buffer, u32>;

    explicit TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer);
    ~TileManager();

    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   vk::Buffer out_buffer, u64 out_offset, u32 copy_size);

    Result DetileImage(vk::Buffer in_buffer, u32 in_offset, const ImageInfo& info);

    // Raw-bit packing the fused readback shader is compiled for, chosen from the backing
    // format. kind 0 means the format is not covered and the blit/copy/tile chain is used.
    struct ReadbackPack {
        u32 kind{};
        bool bgra{};
        vk::Format view_format{};
    };

private:
    vk::Pipeline GetTilingPipeline(const ImageInfo& info, bool is_tiler);
    vk::Pipeline GetImageTilingPipeline(const ImageInfo& info, const ReadbackPack& pack);
    // Tiles `num_mips` levels of a scaled image straight into guest layout by sampling
    // the scaled backing. Returns false when the image/format is outside the fused path.
    bool TileImageFromScaled(Image& in_image, u32 num_mips, vk::Buffer out_buffer,
                             u64 out_offset);
    ScratchBuffer GetScratchBuffer(u32 size);

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    StreamBuffer& stream_buffer;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> detilers{};
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> tilers{};
    // Fused readback: sampler-fed tiler pipelines keyed by tile mode, bpp and pack.
    vk::UniqueDescriptorSetLayout image_desc_layout;
    vk::UniquePipelineLayout image_pl_layout;
    vk::UniqueSampler linear_sampler;
    vk::UniqueSampler nearest_sampler;
    std::unordered_map<u32, vk::UniquePipeline> image_tilers;
    bool fused_readback{true};
};

} // namespace VideoCore
