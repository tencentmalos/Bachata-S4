// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include "video_core/amdgpu/regs_primitive.h"

namespace AmdGpu {

// Keep the viewport affine transform separate from the clip volume. Clamping
// its endpoints changes every in-range depth, including sampled scene depth.
struct DepthRangeTransform {
    float scale{};
    float offset{};
    float min_depth{};
    float max_depth{};
    bool operator==(const DepthRangeTransform&) const = default;
};

inline std::array<float, 2> ViewportDepthRange(float scale, float offset, ClipSpace clip) {
    return {clip == ClipSpace::MinusWToW ? offset - scale : offset, offset + scale};
}

inline bool NeedsDepthRangeEmulation(float scale, float offset, ClipSpace clip) {
    const auto range = ViewportDepthRange(scale, offset, clip);
    return range[0] < 0.f || range[0] > 1.f || range[1] < 0.f || range[1] > 1.f;
}

inline DepthRangeTransform MakeDepthRangeTransform(float scale, float offset, float clamp_min,
                                                   float clamp_max) {
    const auto lo = std::min(clamp_min, clamp_max);
    const auto hi = std::max(clamp_min, clamp_max);
    // A zero-width viewport produces constant depth; no division by zero.
    if (lo == hi)
        return {0.f, 0.5f, lo, hi};
    return {scale / (hi - lo), (offset - lo) / (hi - lo), lo, hi};
}

struct DepthRangeEmulation {
    bool enabled{};
    bool negative_one_to_one{};
    bool clip_near{};
    bool clip_far{};
    // Indexed exactly like the emitted Vulkan viewport array. Constants enter
    // the shader specialization key; no guest pointer or per-vertex host call.
    std::array<DepthRangeTransform, NUM_VIEWPORTS> viewports{};
    bool operator==(const DepthRangeEmulation&) const = default;
};

template <class Registers>
DepthRangeEmulation BuildDepthRangeEmulation(const Registers& regs, bool unrestricted) {
    DepthRangeEmulation result{};
    if (unrestricted)
        return result;
    const auto& ctl = regs.viewport_control;
    for (const auto& vp : regs.viewports) {
        if (vp.xscale != 0.f && NeedsDepthRangeEmulation(ctl.zscale_enable ? vp.zscale : 1.f,
                                                         ctl.zoffset_enable ? vp.zoffset : 0.f,
                                                         regs.clipper_control.clip_space)) {
            result.enabled = true;
            break;
        }
    }
    if (!result.enabled)
        return result;
    result.negative_one_to_one = regs.clipper_control.clip_space == ClipSpace::MinusWToW;
    result.clip_near = !regs.clipper_control.zclip_near_disable && !regs.IsClipDisabled();
    result.clip_far = !regs.clipper_control.zclip_far_disable && !regs.IsClipDisabled();
    u32 index = 0;
    for (u32 i = 0; i < NUM_VIEWPORTS; ++i) {
        const auto& vp = regs.viewports[i];
        if (vp.xscale == 0.f)
            continue;
        const bool clamp = !regs.depth_render_override.disable_viewport_clamp;
        result.viewports[index++] = MakeDepthRangeTransform(
            ctl.zscale_enable ? vp.zscale : 1.f, ctl.zoffset_enable ? vp.zoffset : 0.f,
            clamp ? regs.viewport_depths[i].zmin : 0.f, clamp ? regs.viewport_depths[i].zmax : 1.f);
    }
    return result;
}
} // namespace AmdGpu
