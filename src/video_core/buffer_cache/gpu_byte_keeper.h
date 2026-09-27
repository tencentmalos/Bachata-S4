// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include "common/futex_mutex.h"
#include "common/types.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/region_definitions.h"

namespace VideoCore {

/// Keeps the bytes the GPU wrote from being replaced by stale guest memory when the CPU writes
/// other bytes of the same tracking page.
///
/// With readbacks off, GPU results never reach guest memory. A CPU write anywhere on a page that
/// holds GPU results used to mark the whole page CPU modified, and the next GPU use uploaded the
/// whole page from guest memory, replacing the results with stale bytes. Games place CPU
/// constants right next to GPU-written tables in the same per-frame ring (on PS4 the bytes are
/// independent), so this lost GPU data every frame.
///
/// The bytes written by GPU bindings are recorded. When a CPU write faults on such a page:
///  - inside those bytes: the CPU takes the page over as before (whole page uploaded);
///  - outside them: the page contents are snapshotted, and the upload sends the bytes outside
///    the GPU ranges plus the GPU bytes that differ from the snapshot (written by the CPU after
///    the fault). A CPU store of the value guest memory already held is the one case not seen.
/// Fault-side calls take no allocation (signal handler on Android); slots are a fixed pool.
class GpuByteKeeper {
public:
    static constexpr u64 PageSize = TRACKER_BYTES_PER_PAGE;
    static constexpr size_t MaxKeptPages = 64;
    /// Ranges one kept page may split into before it is uploaded whole instead.
    static constexpr u32 MaxRunsPerPage = 8;

    struct Counters {
        std::atomic<u64> kept_pages{};      // CPU faults beside GPU bytes: GPU bytes kept
        std::atomic<u64> taken_pages{};     // CPU faults inside GPU bytes: page taken over
        std::atomic<u64> evicted_pages{};   // kept pages dropped because every slot was in use
        std::atomic<u64> kept_uploads{};    // kept pages uploaded around their GPU bytes
        std::atomic<u64> cpu_rewritten{};   // GPU bytes the CPU rewrote after the fault (uploaded)
        std::atomic<u64> split_fallbacks{}; // kept pages uploaded whole (too many runs/budget)
    };

    static inline std::atomic<bool> enabled{true};

    /// A GPU binding writes [addr, addr + size).
    void NoteGpuWrite(VAddr addr, u64 size) {
        std::scoped_lock lk{mutex};
        written.Add(addr, size);
    }

    /// Guest memory in [addr, addr + size) now matches the GPU (readback) or is gone (unmap).
    void Forget(VAddr addr, u64 size) {
        std::scoped_lock lk{mutex};
        written.Subtract(addr, size);
        for (auto& slot : slots) {
            if (slot.page != 0 && slot.page + PageSize > addr && slot.page < addr + size) {
                slot.page = 0;
            }
        }
    }

    /// Fault side, under the region lock of `page` (page aligned, GPU modified): the CPU writes
    /// [addr, addr + size) of it. Returns true when the page's GPU bytes are kept; false when the
    /// page is taken over as a whole (its GPU data is lost, as without this class).
    bool OnCpuWrite(VAddr page, VAddr addr, u64 size) {
        if (!enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lk{mutex};
        if (!written.Intersects(page, PageSize)) {
            return false;
        }
        if (written.Intersects(addr, size)) {
            // A snapshot from an earlier fault beside the GPU bytes no longer applies either.
            if (Slot* kept = FindSlot(page)) {
                kept->page = 0;
            }
            counters.taken_pages.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Slot* slot = nullptr;
        for (auto& candidate : slots) {
            if (candidate.page == page) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            // Oldest slot when all are in use: its page falls back to a whole upload.
            slot = &slots[0];
            for (auto& candidate : slots) {
                if (candidate.page == 0) {
                    slot = &candidate;
                    break;
                }
                if (candidate.sequence < slot->sequence) {
                    slot = &candidate;
                }
            }
            if (slot->page != 0) {
                counters.evicted_pages.fetch_add(1, std::memory_order_relaxed);
            }
            // The page is write protected and readable: nothing has changed it since it was
            // last uploaded.
            std::memcpy(slot->bytes.data(), reinterpret_cast<const u8*>(page), PageSize);
            slot->page = page;
            slot->sequence = ++sequence;
        }
        counters.kept_pages.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /// Upload side, under the region locks: [begin, begin + size) is a run of CPU modified pages
    /// about to be uploaded from guest memory. Calls emit(addr, size) for the bytes to upload
    /// and drops the GPU ownership of the bytes the CPU takes over. `extra_budget` limits how
    /// many ranges beyond one per call may be produced (the caller's preallocated copies).
    template <typename Emit>
    void ForEachUpload(VAddr begin, u64 size, u32& extra_budget, Emit&& emit) {
        std::scoped_lock lk{mutex};
        if (!written.Intersects(begin, size)) {
            emit(begin, size);
            return;
        }
        // Coalesce whole pages; a kept page flushes the pending run and emits its own ranges.
        VAddr run_begin = begin;
        VAddr run_end = begin;
        const auto flush_run = [&] {
            if (run_end != run_begin) {
                emit(run_begin, run_end - run_begin);
            }
        };
        for (VAddr page = begin; page < begin + size; page += PageSize) {
            Slot* slot = FindSlot(page);
            if (!slot || !written.Intersects(page, PageSize)) {
                if (slot) {
                    slot->page = 0;
                }
                if (run_end != page) {
                    flush_run();
                    run_begin = page;
                }
                run_end = page + PageSize;
                // The page is uploaded whole: whatever the GPU wrote there is replaced.
                written.Subtract(page, PageSize);
                continue;
            }
            std::array<std::pair<VAddr, VAddr>, MaxRunsPerPage> runs;
            u32 num_runs = 0;
            if (!SplitKeptPage(*slot, page, runs, num_runs) || num_runs > extra_budget) {
                counters.split_fallbacks.fetch_add(1, std::memory_order_relaxed);
                slot->page = 0;
                if (run_end != page) {
                    flush_run();
                    run_begin = page;
                }
                run_end = page + PageSize;
                written.Subtract(page, PageSize);
                continue;
            }
            flush_run();
            run_begin = run_end = page + PageSize;
            for (u32 i = 0; i < num_runs; ++i) {
                emit(runs[i].first, runs[i].second - runs[i].first);
            }
            extra_budget -= num_runs;
            counters.kept_uploads.fetch_add(1, std::memory_order_relaxed);
            slot->page = 0;
        }
        if (run_end != run_begin) {
            emit(run_begin, run_end - run_begin);
        }
    }

    const Counters& GetCounters() const {
        return counters;
    }

private:
    struct Slot {
        VAddr page{};
        u64 sequence{};
        std::array<u8, PageSize> bytes{};
    };

    Slot* FindSlot(VAddr page) {
        for (auto& slot : slots) {
            if (slot.page == page) {
                return &slot;
            }
        }
        return nullptr;
    }

    /// The ranges of a kept page to upload: bytes outside the GPU ranges, and GPU bytes that
    /// differ from the fault-time snapshot (compared per dword). The latter leave the GPU set.
    bool SplitKeptPage(const Slot& slot, VAddr page,
                       std::array<std::pair<VAddr, VAddr>, MaxRunsPerPage>& runs, u32& num_runs) {
        std::array<bool, PageSize / 4> upload{};
        const auto* now = reinterpret_cast<const u8*>(page);
        for (auto& flag : upload) {
            flag = true;
        }
        u64 rewritten = 0;
        written.ForEachInRange(page, PageSize, [&](VAddr lo, VAddr hi) {
            for (VAddr dword = lo & ~VAddr{3}; dword < hi; dword += 4) {
                const u64 offset = dword - page;
                const bool changed = std::memcmp(now + offset, slot.bytes.data() + offset, 4) != 0;
                upload[offset / 4] = changed;
                rewritten += changed ? 4 : 0;
            }
        });
        num_runs = 0;
        for (size_t i = 0; i < upload.size();) {
            if (!upload[i]) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < upload.size() && upload[j]) {
                ++j;
            }
            if (num_runs == MaxRunsPerPage) {
                return false;
            }
            runs[num_runs++] = {page + i * 4, page + j * 4};
            i = j;
        }
        // The CPU now owns what it rewrote.
        for (u32 i = 0; i < num_runs; ++i) {
            written.Subtract(runs[i].first, runs[i].second - runs[i].first);
        }
        counters.cpu_rewritten.fetch_add(rewritten, std::memory_order_relaxed);
        return true;
    }

    Common::FutexMutex mutex;
    RangeSet written;
    std::array<Slot, MaxKeptPages> slots{};
    u64 sequence{};
    Counters counters;
};

} // namespace VideoCore
