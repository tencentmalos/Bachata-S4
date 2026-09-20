// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include "video_core/texture_cache/scale_policy.h"

namespace VideoCore::MemoryDiagnostics {
// Readers only request/copy a snapshot. The renderer owns all Vulkan/cache access.
inline std::atomic<unsigned long long> requested{};
inline std::atomic<unsigned long long> completed{};
inline std::mutex mutex;
inline unsigned long long epoch{};
inline bool active{};
inline std::string snapshot;
inline ScalePolicySnapshot scale_policy;
// Allocation events only, never sampled per draw. These count upload source images
// until their scheduler-owned objects are destroyed, including GPU retirement.
inline std::atomic<unsigned long long> upload_image_bytes{};
inline std::atomic<unsigned long long> upload_image_created_bytes{};
inline std::atomic<unsigned long long> upload_image_created_count{};

inline unsigned long long Begin(ScalePolicySnapshot policy = {}) {
    std::scoped_lock lock{mutex};
    ++epoch;
    active = true;
    scale_policy = policy;
    requested.store(0);
    completed.store(0);
    snapshot.clear();
    return epoch;
}

inline std::string PolicyStatus() {
    std::scoped_lock lock{mutex};
    if (!active) return "status=unavailable (no renderer)\n";
    return "render_scale_percent=" + std::to_string(scale_policy.render_eighths * 12.5f) +
        " texture_quality=" + std::string(TextureQualityName(scale_policy.texture)) +
        " legacy=" + (scale_policy.legacy ? "true" : "false") + " restart_required=true\n";
}

inline void End(unsigned long long owner) {
    std::scoped_lock lock{mutex};
    if (owner != epoch) return;
    active = false;
    requested.store(0);
    snapshot.clear();
}

inline void Publish(unsigned long long owner, unsigned long long request, std::string value) {
    std::scoped_lock lock{mutex};
    if (owner == epoch && active) {
        snapshot = std::move(value);
        completed.store(request, std::memory_order_release);
    }
}

inline std::string Read(bool request) {
    std::scoped_lock lock{mutex};
    if (!active) return "status=unavailable (no renderer)\n";
    if (request) requested.fetch_add(1, std::memory_order_release);
    return "cache_epoch=" + std::to_string(epoch) + " request_id=" + std::to_string(requested.load()) +
        " completed_id=" + std::to_string(completed.load()) + " pending=" +
        (requested.load() != completed.load() ? "true\n" : "false\n") +
        (snapshot.empty() ? "status=awaiting-renderer-sample\n" : snapshot);
}
} // namespace VideoCore::MemoryDiagnostics
