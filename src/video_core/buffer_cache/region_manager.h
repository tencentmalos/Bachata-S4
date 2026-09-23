// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"

#ifdef __unix__
#include "common/adaptive_mutex.h"
#endif
// Bionic is Unix but does not provide the GNU adaptive-mutex initializer.
// Keep the fallback declaration available on every platform.
#include "common/spin_lock.h"
#if defined(__ANDROID__)
#include "common/futex_mutex.h"
#endif
#include "common/debug.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

#if defined(__ANDROID__)
// Faulting guest writers sleep while another thread snapshots/tracks this region.
using LockType = Common::FutexMutex;
#elif defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
using LockType = Common::AdaptiveMutex;
#else
using LockType = Common::SpinLock;
#endif

/**
 * Tracks CPU/GPU modification in one TRACKER_HIGHER_PAGE_SIZE address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_) {
        Initialize(tracker_, cpu_addr_);
    }
    explicit RegionManager() = default;

    // Called once before publishing a pooled manager to faulting threads.
    void Initialize(PageManager* tracker_, VAddr cpu_addr_) {
        tracker = tracker_;
        cpu_addr = cpu_addr_;
        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
        written.Clear();
        released_ahead.Clear();
        confidence_lo.Clear();
        confidence_hi.Clear();
    }

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    VAddr GetCpuAddr() const {
        return cpu_addr;
    }

    // Roll back an aborted snapshot while the region is still exclusively held.
    // GPU ownership is published only after every copy in the transaction succeeds.
    void RestoreCpuTracking(const RegionBits& original) {
        cpu = original;
        UpdateProtection<false, false>();
    }

    static constexpr size_t SanitizeAddress(size_t address) {
        return static_cast<size_t>(std::max<s64>(static_cast<s64>(address), 0LL));
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type>
    const RegionBits& GetRegionBits() const noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    /**
     * Change the state of a range of pages
     *
     * @param dirty_addr    Base address to mark or unmark as modified
     * @param size          Size in bytes to mark or unmark as modified
     */
    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, u64 size) noexcept(type == Type::GPU) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::CPU) {
            UpdateProtection<!enable, false>();
        } else if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise) {
            UpdateProtection<enable, true>();
        }
    }

    /**
     * A CPU write fault in [dirty_addr, dirty_addr + size): mark those pages CPU modified and,
     * when predicting, also the run of following pages in the window that are expected to be
     * rewritten (write confidence > 0) and are still watched and not GPU modified. Uploading a
     * predicted page the CPU did not write again only costs a redundant copy of unchanged guest
     * memory, while each avoided fault saves a signal round trip and an mprotect.
     * Returns the number of pages released ahead.
     */
    size_t MarkWriteFault(u64 dirty_addr, u64 size, bool predict) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        if (start_page >= end_page) {
            return 0;
        }
        RegionBits release;
        release.SetRange(start_page, end_page);
        size_t predicted = 0;
        if (predict) {
            // Streaming writers fill buffers front to back: release the confident pages that
            // follow the fault, skipping pages already released and stopping at the first page
            // that is GPU modified or not expected to be written.
            constexpr size_t Window = std::min<size_t>(WRITE_FAULT_WINDOW_PAGES, NUM_PAGES_PER_REGION);
            const size_t window_end = std::min(Common::DivCeil(end_page, Window) * Window,
                                               NUM_PAGES_PER_REGION);
            const RegionBits confident = confidence_lo | confidence_hi;
            for (size_t page = end_page; page < window_end; ++page) {
                if (gpu.Get(page)) {
                    break;
                }
                if (cpu.Get(page)) {
                    continue;
                }
                if (!confident.Get(page)) {
                    break;
                }
                released_ahead.Set(page);
                cpu.Set(page);
                ++predicted;
            }
        }
        written |= release;
        cpu |= release;
        UpdateProtection<false, false>();
        return predicted;
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.
     *
     * @param query_cpu_range Base CPU address to loop over
     * @param size            Size in bytes of the CPU range to loop over
     * @param func            Function to call for each turned off region
     */
    template <Type type, bool clear>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, auto&& func) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        RegionBits mask(bits, start_page, end_page);

        if constexpr (clear) {
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                EndWriteCycle(mask);
                UpdateProtection<true, false>();
            } else if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                UpdateProtection<false, true>();
            }
        }

        for (const auto& [start, end] : mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    /**
     * Returns true when a region has been modified
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        const RegionBits& bits = GetRegionBits<type>();
        return bits.AnyInRange(start_page, end_page);
    }

    LockType lock;

private:
    /**
     * Notify tracker about changes in the CPU tracking state of a word in the buffer
     *
     * @param word_index   Index to the word to notify to the tracker
     * @param current_bits Current state of the word
     * @param new_bits     New state of the word
     *
     * @tparam track True when the tracker should start tracking the new pages
     */
    template <bool track, bool is_read>
    void UpdateProtection() {
        RENDERER_TRACE;
        RegionBits mask = is_read ? (~gpu ^ readable) : (cpu ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {
            writeable = cpu;
        }
        tracker->UpdatePageWatchersForRegion<track, is_read>(cpu_addr, mask);
    }

    /**
     * An upload cleans `cleaned` and re-watches it, ending those pages' write cycle. A released
     * page gives no evidence of whether the CPU wrote it, so the 2-bit write confidence is set
     * to 3 by a real fault, decays by one per cycle spent released ahead and drops to 0 when the
     * page was dirty for another reason. A page the CPU keeps rewriting faults once every four
     * cycles; one it stopped writing is uploaded redundantly at most three more times.
     */
    void EndWriteCycle(const RegionBits& cleaned) {
        const RegionBits faulted = written & cleaned;
        const RegionBits ahead = released_ahead & cleaned & ~faulted;
        const RegionBits lo = confidence_lo, hi = confidence_hi;
        confidence_lo = (lo & ~cleaned) | faulted | (ahead & hi & ~lo);
        confidence_hi = (hi & ~cleaned) | faulted | (ahead & hi & lo);
        written &= ~cleaned;
        released_ahead &= ~cleaned;
    }

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
    RegionBits written;        // CPU write faults since the upload that last cleaned the page
    RegionBits released_ahead; // released by MarkWriteFault prediction in this cycle
    RegionBits confidence_lo;  // 2-bit write confidence (see EndWriteCycle)
    RegionBits confidence_hi;
};

} // namespace VideoCore
