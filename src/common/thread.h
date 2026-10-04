// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

namespace Common {

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

void SetCurrentThreadRealtime(std::chrono::nanoseconds period_ns);

/// Emulator-owned threads: Low for background work (pipeline compilation, cache I/O), High
/// for the threads a frame waits on (GPU command processing, recording, presentation, audio).
/// Applied as a nice offset (see SetThreadNice); a no-op when host thread priorities are
/// disabled.
void SetCurrentThreadPriority(ThreadPriority new_priority);

/// Nice offset (see SetThreadNice) used for a ThreadPriority level.
int ThreadPriorityNice(ThreadPriority priority);

/// A host thread whose scheduling can be changed from any thread of the process: the kernel
/// thread id on Linux/Android, the thread id on Windows, the pthread_t on Apple. 0 is none.
using NativeThreadRef = std::uintptr_t;

NativeThreadRef CurrentNativeThreadRef();

/// Sets a thread's host scheduling weight as an offset on the Linux nice scale: negative gets
/// more CPU time, positive less, 0 leaves the thread where the process started. Stays
/// time-sharing (never a real-time policy).
/// Linux/Android: threads inherit their creator's nice (an Android foreground app starts its
/// threads at -10), so the offset is added to ThreadNiceBase() and applied with setpriority(); a
/// result below the RLIMIT_NICE floor is raised to the floor, and where a thread could not be
/// brought back to the base no change is made. Windows maps the offset onto thread priority
/// levels (<= -6 highest, <= -3 above normal, >= 3 below normal, >= 10 lowest), Apple onto
/// SCHED_OTHER priorities around the default 31.
/// Returns the host value applied (the absolute nice on Linux/Android, the offset elsewhere), or
/// nullopt if nothing was changed (disabled, no thread, refused).
std::optional<int> SetThreadNice(NativeThreadRef thread, int offset);

std::optional<int> SetCurrentThreadNice(int offset);

/// Linux/Android: the nice the process's threads had before the first change, read once from the
/// first thread that asks for one (every change goes through SetThreadNice, so that thread is
/// still unchanged). 0 elsewhere.
int ThreadNiceBase();

/// False when host thread priorities are switched off: Android property
/// debug.shadps4.thread_priority=0, elsewhere environment SHADPS4_THREAD_PRIORITY=0. Read once.
bool HostThreadPrioritiesEnabled();

/// Records the last scheduling change of a thread for the thread_priority DebugBus command and
/// logs the first changes. `source` says what asked for it (a guest policy/priority, a host
/// thread class).
void NoteThreadPriority(NativeThreadRef thread, std::string_view name, std::string_view source,
                        int requested, std::optional<int> applied);

/// DebugBus thread_priority: status | reset.
std::string ThreadPriorityCommand(const std::vector<std::string>& args);

/// Linux/Android: the CPUs of the lowest capacity and the others, from
/// /sys/devices/system/cpu/cpuN/cpu_capacity. Both lists are empty when a capacity is unknown or
/// every CPU has the same one (`note` says which); always empty on other platforms.
struct CpuCapacitySplit {
    std::vector<int> lowest;
    std::vector<int> others;
    std::string note;
};
const CpuCapacitySplit& GetCpuCapacitySplit();

/// Linux/Android: restricts a thread to `cpus`, or lets it run on every CPU when `cpus` is
/// empty. False on failure and on other platforms.
bool SetThreadAffinity(NativeThreadRef thread, const std::vector<int>& cpus);

void SetCurrentThreadName(const char* name);

void SetThreadName(void* thread, const char* name);

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible);

class AccurateTimer {
    std::chrono::nanoseconds target_interval{};
    std::chrono::nanoseconds total_wait{};

    std::chrono::high_resolution_clock::time_point start_time;

public:
    explicit AccurateTimer(std::chrono::nanoseconds target_interval);

    void Start();

    void End();

    std::chrono::nanoseconds GetTotalWait() const {
        return total_wait;
    }
};

std::string GetCurrentThreadName();

} // namespace Common
