//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
namespace Vulkan { class Instance; class Scheduler; }

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        // Bits: 0 = distinct base eye views; 1 = colour overlay enabled;
        // 2 = distinct overlay eye views; 3 = opaque VR presentation.
        // Shared views retain their native UVs;
        // shared identity alone does not establish binocular scene content.
        u32 sbs = 0;
        u32 flip_y = 0;
        // Bit 4 selects these explicit per-eye normalized scale/bias vectors.
        alignas(16) std::array<std::array<float, 4>, 4> eye_uv{};
    };

    static_assert(offsetof(Settings, eye_uv) == 32);
    static_assert(sizeof(Settings) == 96);

    void Create(const Instance& instance, vk::Format surface_format);

    void Render(Scheduler& scheduler, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings,
                std::array<vk::ImageView, 3> stereo_views = {},
                vk::ImageView fdm_view = {},
                std::array<vk::Sampler, 4> stereo_samplers = {});

private:
    vk::Device device{};
    vk::UniquePipeline pipeline{};
    vk::UniquePipeline fdm_pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    vk::UniqueRenderPass fdm_render_pass{};
    bool fdm_dynamic{};
};

} // namespace Vulkan::HostPasses
