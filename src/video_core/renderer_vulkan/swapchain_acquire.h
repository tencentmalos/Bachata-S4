// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

enum class AcquireStatus { Acquired, NotReady, Cancelled, Recreate, Error };

struct AcquireResult {
    AcquireStatus status;
    vk::Result result;
    bool recreate = false;
    bool surface_lost = false;
};

// One driver call per frame lets the outer render loop observe cancellation.
// Success/suboptimal must consume the acquired semaphore even if Stop races
// with the call. Skipping that image would leave a signalled semaphore for reuse.
template <typename Acquire>
AcquireResult AcquireSwapchainImage(const std::atomic<bool>& stop_requested, Acquire&& acquire) {
    if (stop_requested.load(std::memory_order_acquire)) {
        return {AcquireStatus::Cancelled, vk::Result::eNotReady};
    }
    const auto result = acquire(100'000'000ULL);
    switch (result) {
    case vk::Result::eSuccess:
        return {AcquireStatus::Acquired, result};
    case vk::Result::eSuboptimalKHR:
        return {AcquireStatus::Acquired, result, true};
    case vk::Result::eTimeout:
    case vk::Result::eNotReady:
        return {AcquireStatus::NotReady, result};
    case vk::Result::eErrorSurfaceLostKHR:
        return {AcquireStatus::Recreate, result, true, true};
    case vk::Result::eErrorOutOfDateKHR:
        return {AcquireStatus::Recreate, result, true};
    default:
        return {AcquireStatus::Error, result};
    }
}

} // namespace Vulkan
