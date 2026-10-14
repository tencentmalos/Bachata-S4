// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

// Count, total and longest duration of named code sections, for diagnostics that need exact
// in-process numbers without a profiler capture (e.g. `memory_lock status`). Each section name
// is a string literal and owns one slot, claimed on first use; recording is three relaxed
// atomics. Sections past the table size are not recorded. Standard library only, so the
// backend-free guest_cpu_api target can use it too.
namespace Common::StepStats {

struct Entry {
    std::atomic<const char*> name{};
    std::atomic<std::uint64_t> count{}, ns{}, max_ns{};
};
constexpr std::size_t MaxEntries = 48;
inline std::array<Entry, MaxEntries> entries{};

inline Entry* For(const char* name) {
    for (auto& entry : entries) {
        const char* current = entry.name.load(std::memory_order_acquire);
        if (current == name) {
            return &entry;
        }
        if (!current) {
            if (entry.name.compare_exchange_strong(current, name, std::memory_order_acq_rel) ||
                current == name) {
                return &entry;
            }
        }
    }
    return nullptr;
}

inline void Add(Entry* entry, std::uint64_t ns) {
    if (!entry) {
        return;
    }
    entry->count.fetch_add(1, std::memory_order_relaxed);
    entry->ns.fetch_add(ns, std::memory_order_relaxed);
    std::uint64_t max = entry->max_ns.load(std::memory_order_relaxed);
    while (ns > max && !entry->max_ns.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
    }
}

/// Records a plain value (for example a container size) under `name`. Report() prints it in the
/// microsecond columns, so avg_us is the average value and max_us the largest.
inline void Value(const char* name, std::uint64_t value) {
    Add(For(name), value * 1000);
}

/// Records the time from construction to Stop() (or destruction) under `name`.
class Timer {
public:
    explicit Timer(const char* name)
        : entry{For(name)}, start{std::chrono::steady_clock::now()} {}
    ~Timer() {
        Stop();
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    void Stop() {
        if (!entry) {
            return;
        }
        Add(entry, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count()));
        entry = nullptr;
    }

private:
    Entry* entry;
    std::chrono::steady_clock::time_point start;
};

inline void Reset() {
    for (auto& entry : entries) {
        entry.count = 0;
        entry.ns = 0;
        entry.max_ns = 0;
    }
}

/// One line per section: name, count, total, average and longest time in microseconds.
inline std::string Report() {
    std::string out;
    for (const auto& entry : entries) {
        const char* name = entry.name.load(std::memory_order_acquire);
        if (!name) {
            break;
        }
        const auto count = entry.count.load(std::memory_order_relaxed);
        const auto ns = entry.ns.load(std::memory_order_relaxed);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "time %s count=%llu total_us=%llu avg_us=%.1f max_us=%llu\n", name,
                      static_cast<unsigned long long>(count),
                      static_cast<unsigned long long>(ns / 1000), count ? ns / 1000.0 / count : 0.0,
                      static_cast<unsigned long long>(
                          entry.max_ns.load(std::memory_order_relaxed) / 1000));
        out += line;
    }
    return out;
}

} // namespace Common::StepStats
