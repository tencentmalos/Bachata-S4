// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include "common/types.h"

namespace Core::GuestCpu {
class GuestAddressSpace;
}
namespace Core::HostRuntime {
inline constexpr std::string_view AvPlayerNids[]{
    "KMcEa+rHsIo", "x8uvuFOPZhU", "NkJwDzKmIlw", "wwM99gjFf1Y", "ODJK2sn9w4A", "Wnp1OVcrZgk",
    "d8FcbzfAdQw", "o3+RWnHViSg", "JdksQu8pNdQ", "aS66RI0gGgo", "o9eWRkSL+M4", "UbQoYawOsfY",
    "9y5v+fGN4Wk", "HD1YKVU26-M", "w5moABNwnRY", "k-q+xOxdc3E", "eBTreZ84JFY", "OVths0xGfho",
    "ET4Gr-Uu07s", "ZC17w3vB5Lo", "hdTyRzCXQeQ"};
bool IsAvPlayerNid(std::string_view nid);

// The native decoder sees only host buffers and host callbacks. Each player has
// one persistent guest callback owner; no VM pin/transaction spans a callback.
class GuestAvPlayer {
public:
    static constexpr size_t ScratchSize = 256 << 10;
    struct Callbacks {
        std::function<u64(u64)> begin; // callback worker: attach, return guest scratch VA
        std::function<u64(u64, std::span<const u64>)> call;
        std::function<void()> cancel;  // any thread, nonblocking guest interrupt
        std::function<void()> end;     // callback worker: detach and release its VM resources
        std::function<void(u64, size_t)> invalidate; // short guest output publication
    };
    GuestAvPlayer(GuestCpu::GuestAddressSpace&, std::recursive_mutex&, std::function<Callbacks()>);
    ~GuestAvPlayer();
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& args);
    void RequestStop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
