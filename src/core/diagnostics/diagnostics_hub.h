// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DiagnosticsHub: the single, process-owned control and status entry for the
// Android graphics/performance debugging toolkit (spec
// docs/specs/android-graphics-debugging-toolkit.md §3.1).
//
// The problem this solves: GuestRuntime::Diagnostics() takes threads_mutex and
// graphics_mutex. Calling it from Android Service.dump (the main thread) would
// let a status query block behind the Session mutex, a VM drain, or a GPU fence
// -- exactly what §3.1 forbids, and a direct path to an ANR. The hub instead
// keeps a PUBLISHED SNAPSHOT the session generation writes into: the numeric
// advance signals are lock-free atomics, and the few short identity strings are
// guarded by a dedicated mutex that is ONLY ever held for a string copy -- never
// across a Session mutex, VM drain, GPU fence, or export. So a busy or wedged
// renderer can never stall a status read, and the string lock cannot deadlock
// against the runtime's locks because the hub never calls back into the runtime
// while holding it.
//
// Design rules enforced here:
//   * Reading a snapshot never blocks on runtime work. Numeric fields are copied
//     from atomics; strings are copied under a microsecond-scale dedicated lock.
//     No guest pointer is touched.
//   * Identity is by generation. A generation owns the publisher; a reader always
//     sees which generation the snapshot belongs to, and a stale generation's
//     late writes are visibly attributed, never silently applied to a newer one.
//   * Advance signals are counted SEPARATELY (spec §3.1): guest flip, PM4
//     consumed, host draw/dispatch, queue submit, GPU retire, host present,
//     overlay redraw, each with its own last-advance timestamp. A signal the
//     build cannot measure is UNAVAILABLE, never inferred as 0.
//
// This header is the status half of §3.1. The command registry
// (debug_status/renderdoc_*/profiler_* ...) is a separate later piece that reads
// through this same hub, so ImGui and adb share one typed backend.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "common/types.h"

namespace Core::Diagnostics {

// The distinct advance signals of §3.1. Order is stable; used to index the
// counter array. Keep Count last.
enum class AdvanceSignal : u32 {
    GuestFlip = 0,   // guest accepted a flip (sceVideoOutSubmitFlip 等)
    Pm4Consumed,     // command processor consumed PM4
    HostDraw,        // host draw/dispatch recorded
    QueueSubmit,     // host queue submit
    GpuRetire,       // verifiable GPU retire (may be UNAVAILABLE)
    HostPresent,     // host present succeeded (Presenter::Present true)
    OverlayRedraw,   // debug overlay redraw (NOT a guest flip)
    GuestSubmission, // accepted GNM submission, distinct from Vulkan queue submit
    Count,
};

inline constexpr u32 kAdvanceSignalCount = static_cast<u32>(AdvanceSignal::Count);

[[nodiscard]] constexpr const char* ToString(AdvanceSignal s) noexcept {
    switch (s) {
    case AdvanceSignal::GuestFlip:
        return "guest_flip";
    case AdvanceSignal::Pm4Consumed:
        return "pm4_consumed";
    case AdvanceSignal::HostDraw:
        return "host_draw";
    case AdvanceSignal::QueueSubmit:
        return "queue_submit";
    case AdvanceSignal::GpuRetire:
        return "gpu_retire";
    case AdvanceSignal::HostPresent:
        return "host_present";
    case AdvanceSignal::OverlayRedraw:
        return "overlay_redraw";
    case AdvanceSignal::GuestSubmission:
        return "guest_submission";
    case AdvanceSignal::Count:
        break;
    }
    return "invalid";
}

// One advance signal's published state. count==0 with available==true means "we
// can measure this and it has genuinely not advanced"; available==false means
// "this build/session cannot measure it" -- callers must render these
// differently (spec §3.1: never report 0 for an unmeasurable signal).
struct AdvanceCounter final {
    u64 count{0};
    u64 last_advance_ns{0};  // monotonic time of the most recent advance; 0 = never
    bool available{true};
};

// A plain-values snapshot returned to a reader. No atomics, no pointers -- safe to
// copy, format, and hand to another thread.
struct DiagnosticsSnapshot final {
    bool has_session{false};
    u64 generation{0};
    u64 pid{0};
    u32 phase{0};  // Core::HostRuntime::Phase ordinal, copied as a scalar

    std::string run_uuid;
    std::string stage;        // free-form most-recent stage label
    std::string stop_reason;  // set once a stop is initiated/known
    std::string driver_identity;
    std::string terminal_detail;
    u64 snapshot_ns{0};       // when this snapshot was taken (monotonic)

    std::array<AdvanceCounter, kAdvanceSignalCount> counters{};

    [[nodiscard]] const AdvanceCounter& Counter(AdvanceSignal s) const {
        return counters[static_cast<u32>(s)];
    }
};

// The publisher a session generation writes into. Numeric fields are lock-free
// atomics; identity strings are guarded by a small dedicated mutex held only for
// a copy. One instance is owned by the hub per active generation.
class DiagnosticsPublisher final {
public:
    DiagnosticsPublisher() = default;
    DiagnosticsPublisher(const DiagnosticsPublisher&) = delete;
    DiagnosticsPublisher& operator=(const DiagnosticsPublisher&) = delete;

    void SetIdentity(u64 generation, u64 pid) noexcept {
        generation_.store(generation, std::memory_order_release);
        pid_.store(pid, std::memory_order_relaxed);
    }
    void Complete() noexcept { active_.store(false, std::memory_order_release); }
    void SetPhase(u32 phase_ordinal) noexcept {
        phase_.store(phase_ordinal, std::memory_order_relaxed);
    }
    // Marks a signal measurable/unmeasurable. GPU retire without timestamp support
    // must MarkAvailable(GpuRetire, false) so readers see UNAVAILABLE, not 0.
    void MarkAvailable(AdvanceSignal s, bool available) noexcept {
        slot(s).available.store(available, std::memory_order_relaxed);
    }
    // Records delta advances of a signal at monotonic time now_ns.
    void Advance(AdvanceSignal s, u64 now_ns, u64 delta = 1) noexcept {
        if (!active_.load(std::memory_order_acquire)) return;
        auto& c = slot(s);
        c.count.fetch_add(delta, std::memory_order_relaxed);
        c.last_advance_ns.store(now_ns, std::memory_order_relaxed);
    }
    // Publishes an absolute count (e.g. mirroring an existing atomic such as
    // VideoOutDriver::guest_presents) and only bumps the timestamp when it moved
    // forward.
    void PublishCount(AdvanceSignal s, u64 absolute, u64 now_ns) noexcept {
        if (!active_.load(std::memory_order_acquire)) return;
        auto& c = slot(s);
        const u64 prev = c.count.exchange(absolute, std::memory_order_relaxed);
        if (absolute > prev) {
            c.last_advance_ns.store(now_ns, std::memory_order_relaxed);
        }
    }

    void SetRunUuid(std::string_view uuid);
    void SetStage(std::string_view stage);
    void SetStopReason(std::string_view reason);
    void SetDriverIdentity(std::string_view identity);
    void SetTerminalDetail(std::string_view detail);

    // Copies numeric fields from atomics and strings under the string lock. Never
    // blocks on runtime work. now_ns stamps the snapshot time.
    void CopyInto(DiagnosticsSnapshot& out, u64 now_ns) const;

    [[nodiscard]] u64 Generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

private:
    struct AtomicCounter {
        std::atomic<u64> count{0};
        std::atomic<u64> last_advance_ns{0};
        std::atomic<bool> available{true};
    };
    AtomicCounter& slot(AdvanceSignal s) noexcept {
        return counters_[static_cast<u32>(s)];
    }
    const AtomicCounter& slot(AdvanceSignal s) const noexcept {
        return counters_[static_cast<u32>(s)];
    }

    std::atomic<bool> active_{true};
    std::atomic<u64> generation_{0};
    std::atomic<u64> pid_{0};
    std::atomic<u32> phase_{0};
    std::array<AtomicCounter, kAdvanceSignalCount> counters_{};

    // Short identity strings, updated rarely (once per stage/stop). Guarded by a
    // dedicated mutex held only for the string assignment/copy -- never across a
    // Session mutex, VM drain, GPU fence, or export.
    mutable std::mutex strings_mtx_;
    std::string run_uuid_;
    std::string stage_;
    std::string stop_reason_;
    std::string driver_identity_;
    std::string terminal_detail_;
};

}  // namespace Core::Diagnostics
