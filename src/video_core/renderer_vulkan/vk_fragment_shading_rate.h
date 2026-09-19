// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// Retain the sample masks: advertising 2x2 at 1x MSAA does not imply 8x support.
struct FragmentShadingRates {
    vk::SampleCountFlags half_samples{};
    vk::SampleCountFlags quarter_samples{};

    vk::Extent2D Select(u32 quality, vk::SampleCountFlagBits samples,
                        bool requires_full_rate = false) const {
        if (!requires_full_rate) {
            if (quality == 0 && (quarter_samples & samples)) {
                return {2, 2};
            }
            if (quality == 1 && (half_samples & samples)) {
                return {2, 1};
            }
        }
        return {1, 1};
    }
};

inline void SetGuestFragmentShadingRate(vk::CommandBuffer cmd, vk::Extent2D size) {
    // Only pipelineFragmentShadingRate is enabled. Both combiners must KEEP
    // (VUIDs 04510/04511); REPLACE would also discard the requested pipeline rate.
    constexpr std::array combiners{vk::FragmentShadingRateCombinerOpKHR::eKeep,
                                   vk::FragmentShadingRateCombinerOpKHR::eKeep};
    cmd.setFragmentShadingRateKHR(size, combiners.data());
}

} // namespace Vulkan
