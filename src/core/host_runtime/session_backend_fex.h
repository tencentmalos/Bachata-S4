// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The guest_cpu_fex-backed ISessionBackend. Declared separately so the JNI layer
// and an optional on-device lifecycle test can construct one, while the host unit
// test keeps using a FakeBackend and never links FEX.

#pragma once

#include "core/host_runtime/session_backend.h"

namespace Core::HostRuntime {

class FexSessionBackend final : public ISessionBackend {
public:
    FexSessionBackend() = default;

    [[nodiscard]] Core::GuestCpu::Result<std::shared_ptr<SessionRuntime>> Prepare(
        const SessionParams& params) override;
    [[nodiscard]] RunReport Run(SessionRuntime& runtime) override;
    [[nodiscard]] Core::GuestCpu::Result<StopTicket> RequestCancel(
        SessionRuntime& runtime) override;
    [[nodiscard]] Core::GuestCpu::Status WaitStopped(SessionRuntime& runtime,
                                                     const StopTicket& ticket,
                                                     std::uint64_t timeout_ns) override;
    void Destroy(SessionRuntime& runtime) override;
};

}  // namespace Core::HostRuntime
