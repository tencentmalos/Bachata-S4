// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "video_core/renderer_vulkan/swapchain_acquire.h"

int main() {
    using namespace Vulkan;
    int checks = 0, failures = 0;
    const auto check = [&](bool good, const char* name) {
        ++checks;
        if (!good) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    std::atomic<bool> stopped{true};
    int calls = 0;
    auto invoke = [&](vk::Result result, bool stop_in_call = false) {
        return AcquireSwapchainImage(stopped, [&](auto timeout) {
            ++calls;
            check(timeout > 0 && timeout <= 100'000'000, "finite driver timeout");
            if (stop_in_call) stopped.store(true);
            return result;
        });
    };
    auto result = invoke(vk::Result::eSuccess);
    check(result.status == AcquireStatus::Cancelled && calls == 0 && !result.recreate,
          "stop before acquire never calls the driver or recreates");
    stopped = false;
    for (const auto driver_result : {vk::Result::eTimeout, vk::Result::eNotReady}) {
        const int before = calls;
        result = invoke(driver_result);
        check(calls == before + 1 && result.status == AcquireStatus::NotReady && !result.recreate,
              "no available image returns to render loop after exactly one call");
    }
    for (const auto driver_result : {vk::Result::eSuccess, vk::Result::eSuboptimalKHR}) {
        stopped = false;
        result = invoke(driver_result, true);
        check(result.status == AcquireStatus::Acquired,
              "stop during successful acquire must still consume the image/semaphore");
        check(result.recreate == (driver_result == vk::Result::eSuboptimalKHR),
              "suboptimal defers rebuild until after acquired image is consumed");
    }
    stopped = false;
    result = invoke(vk::Result::eErrorOutOfDateKHR);
    check(result.status == AcquireStatus::Recreate && result.recreate && !result.surface_lost,
          "out-of-date requires swapchain rebuild");
    result = invoke(vk::Result::eErrorSurfaceLostKHR);
    check(result.status == AcquireStatus::Recreate && result.recreate && result.surface_lost,
          "lost surface requires surface rebuild");
    for (const auto driver_result : {vk::Result::eErrorDeviceLost, vk::Result::eErrorUnknown}) {
        result = invoke(driver_result);
        check(result.status == AcquireStatus::Error && result.result == driver_result && !result.recreate,
              "driver failure preserves the actual error without a rebuild loop");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
