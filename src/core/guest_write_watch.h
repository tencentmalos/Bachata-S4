// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <vector>
#include "common/types.h"

namespace Core::GuestWriteWatch {

// Diagnostic (default off): watches the host writes that put GPU-produced data into guest
// memory (MemoryManager::TryWriteBacking) for a 64-bit pattern and/or a guest range, and
// records who wrote it. Meant for heap corruption by stale readbacks.

extern std::atomic<bool> armed;

// Names the host writer for writes on this thread while alive.
class Scope {
public:
    explicit Scope(const char* writer, u64 detail = 0) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* previous_writer;
    u64 previous_detail;
};

// Called with the destination and bytes about to be written.
void CheckSlow(VAddr destination, const void* data, u64 size);

inline void Check(VAddr destination, const void* data, u64 size) {
    if (armed.load(std::memory_order_relaxed)) [[unlikely]] {
        CheckSlow(destination, data, size);
    }
}

// DebugBus: start [pattern_hex] | range <address> <bytes> | status | dump [count] | stop
std::string Command(const std::vector<std::string>& args);

// Arms at renderer start when SHADPS4_GUEST_WRITE_WATCH / debug.shadps4.guest_write_watch is
// "1" (default pattern) or a hex pattern, so writes during loading are covered.
void ArmFromEnvironment();

} // namespace Core::GuestWriteWatch
