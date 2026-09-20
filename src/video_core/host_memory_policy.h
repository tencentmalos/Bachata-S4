// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/texture_cache/scale_policy.h"

namespace VideoCore {
// Session policy: allocation granularity and cache retention, not a hard memory
// limit. Required/dedicated allocations can exceed the preferred VMA block size.
struct HostMemoryPolicy {
    TextureQuality quality = TextureQuality::High;
    u64 preferred_block_bytes = 256ull << 20;
    u64 staging_bytes = 512ull << 20;
    u64 idle_asset_submits = 0;
    u32 idle_evictions_per_sweep = 0;
    bool compact_allocations = false;

    static constexpr HostMemoryPolicy For(ScalePolicySnapshot scale) {
        if (scale.legacy || scale.texture == TextureQuality::High)
            return {};
        if (scale.texture == TextureQuality::Medium)
            return {TextureQuality::Medium, 128ull << 20, 256ull << 20, 600, 4, true};
        return {TextureQuality::Low, 64ull << 20, 128ull << 20, 180, 8, true};
    }
};
} // namespace VideoCore
