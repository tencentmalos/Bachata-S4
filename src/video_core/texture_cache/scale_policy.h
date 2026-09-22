// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string_view>
#include "common/types.h"
#include "video_core/texture_cache/internal_scale.h"

namespace VideoCore {
enum class TextureQuality : u32 { High, Medium, Low };
inline constexpr std::string_view TextureQualityName(TextureQuality quality) {
    return quality == TextureQuality::Medium ? "medium"
           : quality == TextureQuality::Low  ? "low"
                                             : "high";
}
struct ScalePolicySnapshot {
    u32 render_eighths = 8;
    TextureQuality texture = TextureQuality::High;
    bool legacy = false; // Diagnostic comparison only; never a public preset.
    // Diagnostic A/B knob (debug.shadps4.scale_side_effect_passes=1): a fragment shader
    // that writes storage *buffers* no longer forces the whole pass native. Storage
    // image writes, MSAA and native attachments still do. Fragment-count-dependent
    // buffer contents then differ from native; that is the measured trade-off.
    bool scale_side_effect_passes = false;
    bool ShaderMapping() const {
        return render_eighths != 8 || texture != TextureQuality::High;
    }
};
enum class ScaleUse : u32 { Unknown, Texture, Storage, RenderTarget, DepthTarget, VideoOut };
enum class ScaleDomain : u32 { Unknown, Asset, Render, NativeRequired };
enum class ScaleOrigin : u32 { Unknown, Upload, Render, Compute, Copy };
enum class ScaleReason : u32 {
    None,
    SizeProtect,
    SemanticNative,
    UnknownUsage,
    Alias,
    InsufficientMips,
    MixedPass,
    Readback,
    CopyInherit,
    UpdateCost,
    Budget,
    Legacy,
    Streaming,
    Count
};
inline constexpr std::string_view ScaleReasonName(ScaleReason reason) {
    constexpr std::array names{"none",          "size-protect", "semantic-native",
                               "unknown-usage", "alias",        "insufficient-mips",
                               "mixed-pass",    "readback",     "copy-inherit",
                               "update-cost",   "budget",       "legacy",
                               "streaming"};
    return names[static_cast<size_t>(reason)];
}
inline constexpr std::string_view ScaleDomainName(ScaleDomain domain) {
    constexpr std::array names{"unknown", "asset", "render", "native-required"};
    return names[static_cast<size_t>(domain)];
}
// Includes address/size, format/type, extent/pitch, levels/layers/samples and tiling.
// A table belongs to one cache/session. Unmap retires all overlapping identities;
// monotonically allocated generations prevent a later mapping inheriting history.
using ScaleIdentity = std::array<u64, 12>;
struct ResourceScalePlan {
    ScaleIdentity identity{};
    u64 mapping_generation{}, content_version{}, plan_version{}, source_generation{};
    ScaleDomain domain = ScaleDomain::Unknown;
    ScaleOrigin origin = ScaleOrigin::Unknown;
    ScaleReason reason = ScaleReason::UnknownUsage;
    u32 history{}, eighths = 8, drop{}, physical_width{}, physical_height{}, physical_format{};
    u32 uploads{};
    u64 upload_bytes{};
    u32 native_reason_mask{};
    bool render_transition{}, sampled{}, upscaled_readback{};
    void RequireNative(ScaleReason why) {
        native_reason_mask |= 1u << u32(why);
        if (reason != ScaleReason::None && reason != ScaleReason::UnknownUsage)
            native_reason_mask |= 1u << u32(reason);
        domain = ScaleDomain::NativeRequired;
        reason = why;
        ++plan_version;
    }
};
class ScalePlanTable {
public:
    static constexpr size_t Capacity = 4096;
    std::shared_ptr<ResourceScalePlan> Acquire(const ScaleIdentity& key, ScaleUse use) {
        if (auto it = entries.find(key); it != entries.end()) {
            it->second.stamp = ++clock;
            return it->second.plan;
        }
        bool streaming = false;
        if (use == ScaleUse::Texture) {
            for (const auto& [old, entry] : entries)
                if (old[0] == key[0] && (old[4] != key[4] || old[5] != key[5] || old[8] != key[8]))
                    streaming = true;
        }
        if (entries.size() == Capacity) {
            auto victim = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second.plan.use_count() != 1 ||
                    it->second.plan->domain == ScaleDomain::NativeRequired)
                    continue;
                if (victim == entries.end() || it->second.stamp < victim->second.stamp)
                    victim = it;
            }
            if (victim != entries.end())
                entries.erase(victim);
            else
                budget_native = true;
        }
        auto plan = std::make_shared<ResourceScalePlan>();
        plan->identity = key;
        plan->mapping_generation = ++generation;
        // Once safety history cannot be stored, keep subsequent new identities
        // native for this session. Never evict a sticky safety decision to scale.
        if (budget_native)
            plan->RequireNative(ScaleReason::Budget);
        else if (streaming)
            plan->RequireNative(ScaleReason::Streaming);
        if (entries.size() < Capacity)
            entries.emplace(key, Entry{plan, ++clock});
        return plan;
    }
    void Unmap(u64 address, u64 size) {
        std::erase_if(entries, [&](const auto& pair) {
            const auto& key = pair.first;
            return address < key[0] + key[1] && key[0] < address + size;
        });
    }
    size_t Size() const {
        return entries.size();
    }
    bool BudgetNative() const {
        return budget_native;
    }

private:
    struct Entry {
        std::shared_ptr<ResourceScalePlan> plan;
        u64 stamp;
    };
    std::map<ScaleIdentity, Entry> entries;
    u64 generation{}, clock{};
    bool budget_native{};
};
} // namespace VideoCore
