// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/types.h"

namespace AmdGpu {

/// What an end-of-pipe or end-of-shader event writes when the GPU completes the work before it.
struct Fence {
    enum class Data : u8 {
        None,
        Value,
        GpuClock,
        PerfCounter,
    };
    VAddr address{};
    /// Bytes written (4 or 8); 0 when the event only raises an interrupt.
    u32 size{};
    Data data{Data::None};
    /// Write the guest's backing memory first (its protection may not allow CPU writes), and
    /// only fall back to the virtual address without backing.
    bool through_backing{};
    /// Interrupt to raise after the write, or -1.
    s32 irq{-1};
    u64 value{};
    /// GuestWriteWatch writer name.
    const char* writer{};
    /// The target as the command processor saw it at the packet, pending writes included
    /// (filled by FenceLabels::Add). Another value when the fence takes effect means the guest
    /// wrote the target in between.
    std::array<u32, 2> before{};
};

/// Label and fence writes of end-of-pipe and end-of-shader events (EVENT_WRITE_EOP,
/// EVENT_WRITE_EOS, RELEASE_MEM) that the GPU has not completed yet.
///
/// The guest CPU sees such a write only once the work submitted before the event completed:
/// the scheduler's completion thread performs it then. The command processor runs ahead of the
/// GPU in queue order, so a wait packet on a label of earlier work reads the pending value
/// (Read): on hardware the wait would hold the command processor until that work completed,
/// and the work recorded after the wait runs after it on the same Vulkan queue. A write the
/// command processor makes visible at once after such a wait (WRITE_DATA, a semaphore signal)
/// would then precede the fence, so the command processor performs the fences it waited on
/// first (PerformThrough). Every fence takes effect once, in the order the command processor
/// queued them.
class FenceLabels {
public:
    /// Queues a fence, records its pending value and fills `fence.before`. Returns its sequence
    /// number for PerformThrough.
    u64 Add(Fence fence);

    /// The dword at a 4-byte aligned guest address as the command processor sees it: the
    /// latest pending write, else guest memory. `sequence` receives the sequence number of the
    /// pending fence that provides the value, or 0.
    u32 Read(VAddr address, u64& sequence);

    /// Performs, in order, every queued fence up to `sequence` that has not taken effect yet:
    /// the completion thread once the GPU reached it, or the command processor before a write
    /// that must follow it (`ahead` of the GPU). Any thread.
    void PerformThrough(u64 sequence, bool ahead = false);

    /// Writes a fence and raises its interrupt at once (no deferral). Any thread.
    void PerformNow(const Fence& fence);

    /// Fences queued and not performed yet.
    u32 Outstanding() const {
        return outstanding.load(std::memory_order_acquire);
    }
    /// Fences queued so far. They take effect in this order, so once Performed() reaches a
    /// value read here, every fence queued before the read took effect.
    u64 Deferred() const {
        return deferred.load(std::memory_order_acquire);
    }
    u64 Performed() const {
        return performed.load(std::memory_order_acquire);
    }
    /// Blocks until Performed() reaches `mark`, `stop` is set (checked every few ms) or
    /// `deadline` passes. Returns whether the mark was reached.
    bool WaitPerformed(u64 mark, const std::atomic<bool>& stop,
                       std::chrono::steady_clock::time_point deadline);

    std::string Status() const;

    std::atomic<u64> flushes_burst{};
    std::atomic<u64> flushes_idle{};
    std::atomic<u64> flushes_irq{};
    std::atomic<u64> pending_reads{};
    /// Fences the command processor performed before the GPU completed them (PerformThrough).
    std::atomic<u64> early{};
    /// Fences whose target the guest had changed when they took effect (see Fence::before).
    std::atomic<u64> overwritten{};

private:
    void Write(const Fence& fence, u64 sequence);
    void NoteOverwritten(const Fence& fence, u32 dword, u32 now);

    struct Entry {
        u32 value;
        u64 sequence;
    };
    mutable std::mutex mutex;
    /// By dword address.
    std::unordered_map<VAddr, Entry> pending;
    /// Queued fences in sequence order (guarded by `mutex`).
    std::deque<std::pair<u64, Fence>> queue;
    u64 next_sequence{};
    /// Held while performing, so fences take effect in sequence order whichever thread
    /// performs them.
    std::mutex perform_mutex;
    std::atomic<u32> outstanding{};
    std::atomic<u64> deferred{};
    std::atomic<u64> performed{};
    std::mutex wait_mutex;
    std::condition_variable performed_cv;
    std::atomic<u32> waiters{};
};

} // namespace AmdGpu
