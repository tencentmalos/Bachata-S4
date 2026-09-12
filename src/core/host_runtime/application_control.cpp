// SPDX-License-Identifier: GPL-2.0-or-later
#include <mutex>
#include "core/host_runtime/application_control.h"

namespace Core::HostRuntime {
namespace {
std::mutex control_mutex;
std::shared_ptr<ApplicationControl> current_control;
} // namespace

bool BindApplicationControl(const std::shared_ptr<ApplicationControl>& control) {
    if (!control)
        return false;
    std::scoped_lock lock{control_mutex};
    if (current_control)
        return false;
    current_control = control;
    return true;
}

bool UnbindApplicationControl(const std::shared_ptr<ApplicationControl>& expected) {
    std::shared_ptr<ApplicationControl> retired;
    {
        std::scoped_lock lock{control_mutex};
        if (!expected || current_control != expected)
            return false;
        retired = std::move(current_control);
    }
    return true;
}

LoadExecResult RequestLoadExec(const LoadExecRequest& request) noexcept {
    if (request.guest_path.empty() || request.guest_path.front() != '/') {
        return LoadExecResult::InvalidRequest;
    }
    try {
        std::shared_ptr<ApplicationControl> control;
        {
            std::scoped_lock lock{control_mutex};
            control = current_control;
        }
        return control ? control->RequestLoadExec(request) : LoadExecResult::Unavailable;
    } catch (...) {
        return LoadExecResult::Failed;
    }
}
} // namespace Core::HostRuntime
