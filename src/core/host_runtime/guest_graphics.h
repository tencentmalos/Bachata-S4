// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <functional>
#include "common/types.h"

namespace Frontend { class Window; }
namespace Vulkan { struct Driver; }
namespace Libraries::VideoOut { class VideoOutDriver; }

namespace Core::HostRuntime {
// Owned by the production runtime, inside its exclusive desktop-service lease.
// GPU/VideoOut workers retire before the guest VM and its singleton bindings.
class GuestGraphics final {
public:
    GuestGraphics(std::shared_ptr<Frontend::Window> window,
                  std::shared_ptr<const Vulkan::Driver> driver,
                  std::function<u64()> process_time, std::function<u64()> tsc,
                  std::function<bool()> splash_visible);
    ~GuestGraphics();
    GuestGraphics(const GuestGraphics&) = delete;
    GuestGraphics& operator=(const GuestGraphics&) = delete;
    void RequestStop();
    Libraries::VideoOut::VideoOutDriver& VideoOut();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
