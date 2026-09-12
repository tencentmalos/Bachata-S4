// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Core::HostRuntime {
enum class LoadExecResult { Accepted, Unavailable, Busy, InvalidRequest, Failed };

struct LoadExecRequest {
    std::string guest_path;
    std::vector<std::string> args;
};

class ApplicationControl {
public:
    virtual ~ApplicationControl() = default;
    // Enqueue a generation-bound restart in SessionCore; never exit/fork here.
    virtual LoadExecResult RequestLoadExec(const LoadExecRequest& request) = 0;
};

// The session binds before guest admission and unbinds after guest/control drain.
// Calls retain the controller while dispatching; the controller must own or
// safely reference its generation's session. A later generation cannot be
// withdrawn by an old controller's unbind.
bool BindApplicationControl(const std::shared_ptr<ApplicationControl>& control);
bool UnbindApplicationControl(const std::shared_ptr<ApplicationControl>& expected);
LoadExecResult RequestLoadExec(const LoadExecRequest& request) noexcept;
} // namespace Core::HostRuntime
