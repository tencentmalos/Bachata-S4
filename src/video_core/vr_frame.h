// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include "video_core/amdgpu/resource.h"

namespace VideoCore {
// Owned source snapshot. Each eye has explicit normalized UV scale/bias;
// sharing an image or a slice does not determine the viewport within it.
struct VrFrameSource {
    std::array<AmdGpu::Image, 4> eyes{};
    std::array<AmdGpu::Sampler, 4> samplers{};
    std::array<std::array<float, 4>, 4> uv{};
    u32 image_count{};
};
} // namespace VideoCore
