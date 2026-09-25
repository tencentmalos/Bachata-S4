// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include "core/diagnostics/overlay_control.h"

namespace Core::Diagnostics {
OverlayMailbox& StatusOverlayMailbox() {
    static OverlayMailbox mailbox;
    return mailbox;
}
std::uint64_t OverlayMailbox::Begin() {
    std::lock_guard lock(mutex);
    active = true;
    regions.clear();
    captured.clear();
    touches.clear();
    return ++owner;
}
void OverlayMailbox::End(std::uint64_t value) {
    std::lock_guard lock(mutex);
    if (value != owner)
        return;
    active = false;
    regions.clear();
    captured.clear();
    touches.clear();
    commands.clear();
    pending.store(false, std::memory_order_release);
}
void OverlayMailbox::Publish(std::uint64_t value, std::vector<OverlayRect> next) {
    std::lock_guard lock(mutex);
    if (active && value == owner)
        regions = std::move(next);
}
bool OverlayMailbox::Touch(int id, int phase, float x, float y) {
    std::lock_guard lock(mutex);
    if (!active || phase < 0 || phase > 3 || !std::isfinite(x) || !std::isfinite(y))
        return false;
    auto found = std::find(captured.begin(), captured.end(), id);
    if (phase == 0 && found == captured.end()) {
        const bool hit = std::any_of(regions.begin(), regions.end(), [&](const auto& r) {
            return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
        });
        if (!hit || captured.size() >= 32)
            return false;
        captured.push_back(id);
    } else if (found == captured.end())
        return false;
    if (touches.size() >= 256) {
        touches.clear();
        touches.push_back({-1, 3, 0, 0});
        // Keep admission until each physical Up: never leak the tail of a UI gesture to the game.
    }
    touches.push_back({id, phase, x, y});
    if (phase >= 2)
        std::erase(captured, id);
    pending.store(true, std::memory_order_release);
    return true;
}
void OverlayMailbox::Cancel() {
    std::lock_guard lock(mutex);
    touches.clear();
    touches.push_back({-1, 3, 0, 0});
    captured.clear();
    pending.store(true, std::memory_order_release);
}
void OverlayMailbox::SetPixelDensity(float density) {
    if (std::isfinite(density) && density > 0.f)
        pixel_density.store(std::clamp(density, .5f, 4.f), std::memory_order_relaxed);
}
void OverlayMailbox::Request(std::string command) {
    std::lock_guard lock(mutex);
    if (commands.size() >= 32)
        commands.erase(commands.begin());
    commands.push_back(std::move(command));
    pending.store(true, std::memory_order_release);
}
void OverlayMailbox::Drain(std::vector<OverlayTouch>& out, std::vector<std::string>& requests) {
    std::lock_guard lock(mutex);
    out.swap(touches);
    touches.clear();
    requests.swap(commands);
    commands.clear();
    pending.store(false, std::memory_order_release);
}
} // namespace Core::Diagnostics
