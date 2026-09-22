// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;
class TimelineCompletion;

class Semaphore {
public:
    explicit Semaphore(const Instance& instance_);
    ~Semaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    // Conservative retirement watermark: both GPU complete and host submit returned.
    [[nodiscard]] u64 KnownGpuTick() const noexcept;

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }
    u64 DiagnosticId() const { return diagnostic_id; }

    /// Refresh the known GPU tick
    void Refresh();
    // Call only after the queue accepted the signal operation.
    void Submitted(u64 tick);

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);
    bool Wait(u64 tick, std::stop_token stop);

protected:
    const Instance& instance;
    const u64 diagnostic_id;
    vk::UniqueSemaphore semaphore;    ///< Timeline semaphore.
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.
#ifdef __ANDROID__
    std::unique_ptr<TimelineCompletion> completion;
#endif
};

} // namespace Vulkan
