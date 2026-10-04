// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstring>
#include <fmt/format.h>

#include "common/logging/log.h"

#include "core/guest_write_watch.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/fence_labels.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace AmdGpu {

u64 FenceLabels::Add(Fence fence) {
    std::scoped_lock lock{mutex};
    const u64 sequence = ++next_sequence;
    for (u32 offset = 0; offset < fence.size; offset += sizeof(u32)) {
        const VAddr address = fence.address + offset;
        const auto it = pending.find(address);
        fence.before[offset / sizeof(u32)] =
            it != pending.end() ? it->second.value
                                : *reinterpret_cast<const volatile u32*>(address);
        if (fence.data == Fence::Data::Value) {
            pending[address] = {static_cast<u32>(fence.value >> (offset * 8)), sequence};
        }
    }
    queue.emplace_back(sequence, fence);
    outstanding.fetch_add(1, std::memory_order_acq_rel);
    deferred.fetch_add(1, std::memory_order_acq_rel);
    return sequence;
}

u32 FenceLabels::Read(VAddr address, u64& sequence) {
    {
        std::scoped_lock lock{mutex};
        if (const auto it = pending.find(address); it != pending.end()) {
            pending_reads.fetch_add(1, std::memory_order_relaxed);
            sequence = it->second.sequence;
            return it->second.value;
        }
    }
    // A fence writes guest memory before it forgets its pending value: a value absent here is
    // already in memory.
    sequence = 0;
    return *reinterpret_cast<const volatile u32*>(address);
}

void FenceLabels::PerformThrough(u64 sequence, bool ahead) {
    std::scoped_lock perform{perform_mutex};
    u32 count{};
    for (;;) {
        std::pair<u64, Fence> item;
        {
            std::scoped_lock lock{mutex};
            if (queue.empty() || queue.front().first > sequence) {
                break;
            }
            item = std::move(queue.front());
            queue.pop_front();
        }
        Write(item.second, item.first);
        ++count;
    }
    if (count == 0) {
        return;
    }
    if (ahead) {
        early.fetch_add(count, std::memory_order_relaxed);
    }
    performed.fetch_add(count, std::memory_order_acq_rel);
    outstanding.fetch_sub(count, std::memory_order_acq_rel);
    if (waiters.load(std::memory_order_acquire) != 0) {
        std::scoped_lock lock{wait_mutex};
        performed_cv.notify_all();
    }
}

void FenceLabels::PerformNow(const Fence& fence) {
    Write(fence, 0);
}

void FenceLabels::Write(const Fence& fence, u64 sequence) {
    if (fence.size != 0) {
        u64 value = fence.value;
        if (fence.data == Fence::Data::GpuClock) {
            value = GetGpuClock64();
        } else if (fence.data == Fence::Data::PerfCounter) {
            value = GetGpuPerfCounter();
        }
        void* address = reinterpret_cast<void*>(fence.address);
        if (sequence != 0) {
            for (u32 dword = 0; dword < fence.size / sizeof(u32); ++dword) {
                const u32 now = reinterpret_cast<const volatile u32*>(fence.address)[dword];
                if (now != fence.before[dword]) {
                    NoteOverwritten(fence, dword, now);
                    break;
                }
            }
        }
        const Core::GuestWriteWatch::Scope watch_scope{fence.writer};
        // Outside the table lock: a write to a page the GPU tracks enters the fault handler.
        if (!fence.through_backing ||
            !Core::Memory::Instance()->TryWriteBacking(address, &value, fence.size)) {
            std::memcpy(address, &value, fence.size);
        }
        if (sequence != 0) {
            std::scoped_lock lock{mutex};
            for (u32 offset = 0; offset < fence.size; offset += sizeof(u32)) {
                const auto it = pending.find(fence.address + offset);
                if (it != pending.end() && it->second.sequence == sequence) {
                    pending.erase(it);
                }
            }
        }
    }
    if (fence.irq >= 0) {
        Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(fence.irq));
    }
}

bool FenceLabels::WaitPerformed(u64 mark, const std::atomic<bool>& stop,
                                std::chrono::steady_clock::time_point deadline) {
    if (Performed() >= mark) {
        return true;
    }
    std::unique_lock lock{wait_mutex};
    waiters.fetch_add(1, std::memory_order_acq_rel);
    while (Performed() < mark && !stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        performed_cv.wait_for(lock, std::chrono::milliseconds{5});
    }
    waiters.fetch_sub(1, std::memory_order_acq_rel);
    return Performed() >= mark;
}

void FenceLabels::NoteOverwritten(const Fence& fence, u32 dword, u32 now) {
    const u64 count = overwritten.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 16) {
        LOG_WARNING(Render,
                    "GPU fence {} at {:#x}: dword {} is {:#x} when it takes effect, {:#x} when "
                    "the command processor read the packet (written value {:#x})",
                    fence.writer ? fence.writer : "?", fence.address, dword, now,
                    fence.before[dword], fence.value);
    }
}

std::string FenceLabels::Status() const {
    size_t labels{};
    {
        std::scoped_lock lock{mutex};
        labels = pending.size();
    }
    return fmt::format("deferred={} performed={} early={} outstanding={} pending_dwords={} "
                       "pending_reads={} overwritten={} flushes burst={} idle={} irq={}",
                       deferred.load(), performed.load(), early.load(), outstanding.load(),
                       labels, pending_reads.load(), overwritten.load(), flushes_burst.load(),
                       flushes_idle.load(), flushes_irq.load());
}

} // namespace AmdGpu
