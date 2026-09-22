// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/timeline_completion.h"

#include "common/assert.h"
#include "common/thread.h"
#include "core/diagnostics/pipeline_handoff.h"
#include <stdexcept>

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = 50'000'000;

Semaphore::Semaphore(const Instance& instance_)
    : instance{instance_}, diagnostic_id{Core::Diagnostics::Handoff::NextId()} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
#ifdef __ANDROID__
    // The selected Turnip/KGSL stack can block for a whole frame even in
    // getSemaphoreCounterValue. Keep retirement queries off producer threads.
    // An explicit finite GPU wait establishes completion on this worker instead.
    completion = std::make_unique<TimelineCompletion>(gpu_tick,
        [this, named = false](u64 tick, std::stop_token stop) mutable {
            if (!named) {
                Common::SetCurrentThreadName("shadPS4:GpuDone");
                named = true;
            }
            const vk::SemaphoreWaitInfo info{
                .semaphoreCount = 1, .pSemaphores = &semaphore.get(), .pValues = &tick};
            Core::Diagnostics::Handoff::Scope scope{"Vulkan.CompletionWait",
                instance.DiagnosticGeneration(), diagnostic_id, tick, true};
            while (!stop.stop_requested()) {
                const auto result = instance.GetDevice().waitSemaphores(&info, WAIT_TIMEOUT);
                if (result == vk::Result::eSuccess) return true;
                if (result != vk::Result::eTimeout)
                    throw std::runtime_error("Vulkan completion wait failed: " + vk::to_string(result));
            }
            return false;
        });
#endif
}

Semaphore::~Semaphore() = default;

u64 Semaphore::KnownGpuTick() const noexcept {
#ifdef __ANDROID__
    return completion->ReusableTick();
#else
    return gpu_tick.load(std::memory_order_acquire);
#endif
}

void Semaphore::Refresh() {
    instance.CheckSubmissionHealth();
#ifdef __ANDROID__
    completion->CheckHealth();
#else
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        if (counter_result != vk::Result::eSuccess)
            throw std::runtime_error("Failed to get master semaphore value: " +
                                     vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
#endif
}

void Semaphore::Submitted(u64 tick) {
#ifdef __ANDROID__
    completion->Submitted(tick);
#else
    (void)tick;
#endif
}

void Semaphore::Wait(u64 tick) {
    (void)Wait(tick, {});
}

bool Semaphore::Wait(u64 tick, std::stop_token stop) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return true;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return true;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    Core::Diagnostics::Handoff::Scope scope{"Vulkan.TimelineWait", instance.DiagnosticGeneration(),
        diagnostic_id, tick, true};
    while (true) {
        if (stop.stop_requested()) return false;
        instance.CheckSubmissionHealth();
        const auto result = instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT);
        if (result == vk::Result::eSuccess) break;
        if (result != vk::Result::eTimeout)
            throw std::runtime_error("Vulkan timeline wait failed: " + vk::to_string(result));
    }
#ifdef __ANDROID__
    TimelineCompletion::Complete(gpu_tick, tick);
    if (!completion->WaitSubmitted(tick, stop, [this] { instance.CheckSubmissionHealth(); }))
        return false;
#else
    Refresh();
#endif
    SHAD_HANDOFF(instance.DiagnosticGeneration(), "gpu_completed", diagnostic_id, tick);
    return true;
}

} // namespace Vulkan
