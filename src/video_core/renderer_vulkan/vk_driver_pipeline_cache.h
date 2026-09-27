// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

/// The VkPipelineCache every guest and host pipeline is created with, kept on disk between
/// sessions so the driver does not compile the same shaders to machine code again.
///
/// The file carries its own envelope (format, identity of the device and driver, length and
/// checksum) in front of the driver's data, and the driver data's own header is checked too.
/// Anything that does not match starts an empty cache; the rejected file is kept next to it as
/// `.rejected` for diagnosis. The cache is written by a background thread with an atomic
/// replace: periodically while new pipelines appear (so a killed Android process keeps most of
/// its work), when asked (after a preload), and once more on destruction.
class DriverPipelineCache {
public:
    /// An empty path keeps the cache in memory only.
    DriverPipelineCache(const Instance& instance, std::filesystem::path path);
    ~DriverPipelineCache();

    DriverPipelineCache(const DriverPipelineCache&) = delete;
    DriverPipelineCache& operator=(const DriverPipelineCache&) = delete;

    vk::PipelineCache Handle() const {
        return *cache;
    }

    /// Counts a pipeline created with this cache and schedules a save when enough are new.
    /// Thread-safe.
    void NotePipelineCreated();

    /// Schedules a save now, e.g. after a preload. Thread-safe.
    void RequestSave();

private:
    void Load();
    void Worker(std::stop_token token);
    void Write();

    const Instance& instance;
    const std::filesystem::path path;
    vk::UniquePipelineCache cache{};

    std::mutex mutex{};
    std::condition_variable_any cv{};
    bool save_requested{};
    u32 created_since_request{};
    std::chrono::steady_clock::time_point last_request{};

    std::mutex write_mutex{};
    std::size_t saved_size{};
    std::jthread worker{};
};

} // namespace Vulkan
