// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Core::Diagnostics {
// Metrics and drawing resources remain Presenter-owned. This bounded mailbox
// carries UI/DebugBus requests and normalized touch edges to its render thread.
inline std::atomic<bool> status_overlay_enabled{
#ifdef __ANDROID__
    true
#else
    false
#endif
};
struct OverlayTouch {
    int id{}, phase{};
    float x{}, y{};
}; // down/move/up/cancel = 0/1/2/3
struct OverlayRect {
    float x{}, y{}, width{}, height{};
};
class OverlayMailbox {
public:
    std::uint64_t Begin();
    void End(std::uint64_t owner);
    void Publish(std::uint64_t owner, std::vector<OverlayRect> regions);
    bool Touch(int id, int phase, float x, float y);
    void Cancel();
    void Request(std::string command);
    void Drain(std::vector<OverlayTouch>& touches, std::vector<std::string>& commands);
    void SetPixelDensity(float density);
    float PixelDensity() const {
        return pixel_density.load(std::memory_order_relaxed);
    }
    bool Pending() const {
        return pending.load(std::memory_order_acquire);
    }

private:
    std::mutex mutex;
    std::uint64_t owner{};
    bool active{};
    std::vector<OverlayRect> regions;
    std::vector<int> captured;
    std::vector<OverlayTouch> touches;
    std::vector<std::string> commands;
    std::atomic<bool> pending{};
    std::atomic<float> pixel_density{1.f};
};
OverlayMailbox& StatusOverlayMailbox();
} // namespace Core::Diagnostics
