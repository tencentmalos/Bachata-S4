// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include "core/guest_cpu/debug/target.h"
namespace Core::GuestCpu::Debug {
// One loopback client per context. Stop/join BEFORE destroying the target.
class RspServer final {
public:
    static Result<std::unique_ptr<RspServer>> Listen(Target&, std::uint16_t port);
    ~RspServer();
    std::uint16_t Port() const;

private:
    struct Impl;
    explicit RspServer(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};
} // namespace Core::GuestCpu::Debug
