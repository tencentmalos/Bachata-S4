// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include "common/polyfill_thread.h"

namespace ImGui {
struct StatusMemorySnapshot {
    std::optional<uint64_t> system_used, system_total, system_available;
};

// Sample Android's framework memory API away from the present/XR thread.
// Destroy/join before the snapshot mutex and storage are destroyed.
class StatusMemorySampler {
public:
    StatusMemorySampler();
    StatusMemorySnapshot Latest() const {
        std::lock_guard lock(mutex);
        return latest;
    }
private:
    mutable std::mutex mutex;
    StatusMemorySnapshot latest;
    std::jthread worker;
};
} // namespace ImGui
