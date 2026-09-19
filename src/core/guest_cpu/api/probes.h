// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace Core::GuestCpu {
// Immutable after installation, before the first owner is created. A backend
// emits only listed sites; an absent table adds no generated-code work.
struct ExecutionProbeSite { std::uint64_t pc{}, id{}; };
struct ExecutionProbeOwner {
    virtual ~ExecutionProbeOwner() = default;
    virtual void Hit(std::uint64_t site, std::uint64_t sp) noexcept = 0;
};
class ExecutionProbes {
public:
    virtual ~ExecutionProbes() = default;
    virtual const std::vector<ExecutionProbeSite>& Sites() const noexcept = 0;
    virtual bool ValidateSite(std::uint64_t) { return true; }
    // A physical Run has its own stack, including nested HLE callbacks.
    virtual std::unique_ptr<ExecutionProbeOwner> Enter(std::uint64_t context,
        std::uint64_t thread, std::uint64_t generation, std::uint64_t invocation) = 0;
};
}
