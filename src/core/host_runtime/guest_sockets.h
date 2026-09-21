// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "guest_descriptor_ids.h"
namespace Core::HostRuntime {
// Shared by the POSIX and sceNet ABI adapters. Native sockets are always
// nonblocking; guest blocking uses cancellable readiness waits with no guest pins.
class GuestSockets {
public:
    using Failure = std::function<u64(int)>; // Orbis errno, not host errno
    using FileReady = std::function<int(s32)>;
    explicit GuestSockets(
        std::shared_ptr<GuestDescriptorIds> ids = std::make_shared<GuestDescriptorIds>(),
        bool online = false);
    ~GuestSockets();
    GuestSockets(const GuestSockets&) = delete;
    void RequestStop();
    bool Contains(s32 fd) const;
    bool Empty() const;
    size_t WaiterCount() const;
    static bool IsNetNid(std::string_view nid);
    u64 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& args, const Failure& fail, const FileReady& files = {},
                 std::stop_token stop = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
