//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace Vulkan::HostPasses {

/// Draws the game picture into its rectangle of the window image. It records into the caller's
/// dynamic rendering scope, so the picture and the host overlays drawn after it share one pass
/// over the window image; nothing outside the rectangle is shaded. With `encode` it also applies
/// the post-process transfer, so a guest image reaches the window in this single write.
class PresentPass {
public:
    struct DrawInfo {
        vk::ImageView picture;  // SHADER_READ_ONLY_OPTIMAL
        vk::Rect2D rect;        // stored window-image coordinates
        u32 rotation = 0;       // quarter turns clockwise the window image stores the picture
        bool encode = false;    // linear input: post-process transfer to the window format
        bool srgb_input = false; // sRGB values through a UNORM view: decoded before encoding
        float gamma = 1.0f;
    };

    void Create(const Instance& instance);
    void Draw(Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::Format target_format,
              const DrawInfo& info);

private:
    struct PushConstants {
        u32 rotation;
        u32 encode;
        u32 srgb_input;
        float gamma;
    };

    vk::Pipeline PipelineFor(vk::Format format);

    vk::Device device{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    vk::UniqueShaderModule vs_module{};
    vk::UniqueShaderModule fs_module{};
    /// One pipeline per window-image format (SDR and HDR). Kept for the presenter's lifetime:
    /// a format change never destroys a pipeline that in-flight command buffers may use.
    std::vector<std::pair<vk::Format, vk::UniquePipeline>> pipelines;
};

} // namespace Vulkan::HostPasses
