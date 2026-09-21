// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mutex>
#include "core/libraries/system/commondialog.h"

namespace Core::HostRuntime {
// One CommonDialog control domain per guest session, shared by local providers.
class GuestCommonDialog {
public:
    using Error = Libraries::CommonDialog::Error;
    Error Initialize() {
        std::lock_guard lock(mutex);
        if (initialized)
            return Error::ALREADY_SYSTEM_INITIALIZED;
        initialized = true;
        return Error::OK;
    }
    Error Acquire(const void* requester) {
        std::lock_guard lock(mutex);
        if (!initialized)
            return Error::NOT_SYSTEM_INITIALIZED;
        if (owner)
            return owner == requester ? Error::ALREADY_INITIALIZED : Error::BUSY;
        owner = requester;
        return Error::OK;
    }
    void Release(const void* requester) {
        std::lock_guard lock(mutex);
        if (owner == requester)
            owner = nullptr;
    }
    bool IsUsed() const {
        std::lock_guard lock(mutex);
        return owner != nullptr;
    }

private:
    mutable std::mutex mutex;
    bool initialized{};
    const void* owner{};
};
} // namespace Core::HostRuntime
