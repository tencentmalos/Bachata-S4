// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace Core::HostRuntime::Watchdog {

// What each host thread running guest code is doing, for the dump when the title stops
// presenting frames (AstroQuest import plan WP-I). Every HLE call records its operation and
// the guest stack registers on entry and puts the outer ones back on return, so a reader sees
// the innermost call a thread is in, or 0 while it runs guest code. Single writer per slot,
// relaxed stores: a reader may see a call's operation with the previous call's registers;
// the dump only reads guest memory through checked accesses.
struct Activity {
    std::atomic<std::uint64_t> operation{};
    std::atomic<std::uint64_t> rsp{}, rbp{}, rip{};
    std::atomic<std::uint64_t> calls{}; // HLE entries and returns, to tell progress
    std::atomic<std::uint64_t> guest_thread{};
    std::atomic<int> host_tid{};
    std::atomic<bool> in_use{};
};

inline constexpr std::size_t MaxThreads = 1024;
inline std::array<Activity, MaxThreads> g_slots{};

namespace Detail {
struct Claim {
    Activity* slot{};
    Claim() {
        for (auto& s : g_slots) {
            bool expected = false;
            if (!s.in_use.load(std::memory_order_relaxed) &&
                s.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                s.operation.store(0, std::memory_order_relaxed);
                s.calls.store(0, std::memory_order_relaxed);
                s.guest_thread.store(0, std::memory_order_relaxed);
#if defined(__linux__)
                s.host_tid.store(int(gettid()), std::memory_order_relaxed);
#endif
                slot = &s;
                return;
            }
        }
    }
    ~Claim() {
        if (slot)
            slot->in_use.store(false, std::memory_order_release);
    }
};
} // namespace Detail

// The calling thread's slot; null once all are taken (the thread then goes unreported).
inline Activity* Slot() {
    thread_local Detail::Claim claim;
    return claim.slot;
}

// Scope of one HLE call.
class Entry {
public:
    Entry(std::uint64_t operation, std::uint64_t rsp, std::uint64_t rbp, std::uint64_t rip,
          std::uint64_t guest_thread)
        : slot(Slot()) {
        if (!slot)
            return;
        constexpr auto r = std::memory_order_relaxed;
        prev_operation = slot->operation.load(r);
        prev_rsp = slot->rsp.load(r);
        prev_rbp = slot->rbp.load(r);
        prev_rip = slot->rip.load(r);
        slot->rsp.store(rsp, r);
        slot->rbp.store(rbp, r);
        slot->rip.store(rip, r);
        if (guest_thread && slot->guest_thread.load(r) != guest_thread)
            slot->guest_thread.store(guest_thread, r);
        slot->operation.store(operation, r);
        slot->calls.store(slot->calls.load(r) + 1, r);
    }
    ~Entry() {
        if (!slot)
            return;
        constexpr auto r = std::memory_order_relaxed;
        slot->operation.store(prev_operation, r);
        slot->rsp.store(prev_rsp, r);
        slot->rbp.store(prev_rbp, r);
        slot->rip.store(prev_rip, r);
        slot->calls.store(slot->calls.load(r) + 1, r);
    }
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;

private:
    Activity* slot;
    std::uint64_t prev_operation{}, prev_rsp{}, prev_rbp{}, prev_rip{};
};

// Seconds without a new presented frame before the automatic dump.
inline constexpr unsigned StallSeconds = 20;

struct Control {
    std::atomic<bool> enabled{true};
    std::atomic<bool> dump_requested{};
    std::atomic<std::uint64_t> dumps{}, stalls{};
    std::atomic<std::uint64_t> frames{}, frame_age_ms{};
    std::atomic<bool> session{};
};
inline Control g_control{};

// DebugBus guest_watchdog: status | dump | on | off.
inline std::string Command(const std::vector<std::string>& args) {
    const std::string op = args.empty() ? "status" : args[0];
    if (op == "on" || op == "off") {
        g_control.enabled.store(op == "on");
    } else if (op == "dump") {
        if (!g_control.session.load())
            return "guest_watchdog: no running session\n";
        g_control.dump_requested.store(true);
        return "guest_watchdog: dump requested, written to the log as GUEST_WATCHDOG lines\n";
    } else if (op != "status") {
        return "usage: guest_watchdog status | dump | on | off\n";
    }
    unsigned threads{};
    for (const auto& s : g_slots)
        threads += s.in_use.load(std::memory_order_relaxed);
    return "guest_watchdog: " + std::string(g_control.enabled.load() ? "on" : "off") +
           " stall_after=" + std::to_string(StallSeconds) + "s session=" +
           (g_control.session.load() ? "running" : "none") +
           " frames=" + std::to_string(g_control.frames.load()) +
           " frame_age_ms=" + std::to_string(g_control.frame_age_ms.load()) +
           " stalls=" + std::to_string(g_control.stalls.load()) +
           " dumps=" + std::to_string(g_control.dumps.load()) +
           " threads=" + std::to_string(threads) + "\n";
}

} // namespace Core::HostRuntime::Watchdog
