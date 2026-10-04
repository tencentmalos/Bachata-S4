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
#if defined(__ANDROID__) || defined(_WIN32)
#include "common/futex_mutex.h"
#endif
#include "common/debug.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

#if defined(__ANDROID__) || defined(_WIN32)
// Faulting guest writers sleep while another thread snapshots/tracks this region (the snapshot
// copies memory and changes page protection under the lock; spinning burns the waiter's core).
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
    ~RegionManager() {
        delete content.load(std::memory_order_relaxed);
    }

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
        if (ContentHashes* hashes = content.load(std::memory_order_relaxed)) {
            hashes->valid.Clear();
        }
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
     * Returns the number of pages released ahead; `reached_region_end` (optional) is set when the
     * release ran to the end of the region (only scanned that far with predict_cross_region).
     */
    size_t MarkWriteFault(u64 dirty_addr, u64 size, bool predict,
                          bool* reached_region_end = nullptr) {
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
        auto& counters = content_prediction_counters;
        if (!confidence_lo.Get(start_page) && !confidence_hi.Get(start_page)) {
            counters.fault_cold.fetch_add(1, std::memory_order_relaxed);
            const ContentHashes* hashes = content.load(std::memory_order_acquire);
            if (hashes != nullptr && hashes->valid.Get(start_page)) {
                counters.fault_cold_known.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (start_page > 0 && cpu.Get(start_page - 1)) {
            counters.fault_after_dirty.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters.fault_run_start.fetch_add(1, std::memory_order_relaxed);
            if (start_page % WRITE_FAULT_WINDOW_PAGES == 0) {
                counters.fault_run_start_window.fetch_add(1, std::memory_order_relaxed);
            }
        }
        size_t predicted = 0;
        if (predict) {
            const bool cross = predict_cross_region.load(std::memory_order_relaxed);
            const size_t window_end =
                cross ? NUM_PAGES_PER_REGION
                      : std::min(Common::DivCeil(end_page, ReleaseWindow) * ReleaseWindow,
                                 NUM_PAGES_PER_REGION);
            bool reached_end = false;
            predicted = ReleaseAhead(end_page, window_end, reached_end);
            if (reached_region_end != nullptr) {
                *reached_region_end = cross && reached_end;
            }
        }
        written |= release;
        cpu |= release;
        UpdateProtection<false, false>();
        return predicted;
    }

    /// A release ahead in the previous region ran to its end: continue it over this region's
    /// first window. Returns the number of pages released.
    size_t ContinueReleaseAhead() {
        bool reached_end = false;
        const size_t predicted = ReleaseAhead(0, ReleaseWindow, reached_end);
        if (predicted != 0) {
            UpdateProtection<false, false>();
        }
        return predicted;
    }

    struct UploadPrediction {
        size_t hashed{};
        size_t rewritten{};
        size_t unchanged{};
        size_t kept{};
    };

    /**
     * Upload: clean the CPU modified pages of the range, watch them again and call
     * copy(address, size) for each cleaned run. With `hash_contents`, the pages whose write cycle
     * ends with a fault or a release ahead are then hashed by hash_pages(address, count, hashes)
     * (write protected again, so this is what was uploaded), which tells a page the CPU rewrote
     * while released ahead from one it left alone (see EndWriteCycle).
     */
    UploadPrediction CleanForUpload(VAddr query_cpu_range, s64 size, bool hash_contents,
                                    auto&& copy, auto&& hash_pages) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return {};
        }
        const RegionBits cleaned(cpu, start_page, end_page);
        cpu.UnsetRange(start_page, end_page);
        UpdateProtection<true, false>();

        const RegionBits ended = (written | released_ahead) & cleaned;
        const RegionBits ahead = released_ahead & ended & ~written;
        ContentHashes* hashes = content.load(std::memory_order_acquire);
        if (hash_contents && hashes == nullptr && ended.Any()) {
            wants_content.store(true, std::memory_order_relaxed);
        }
        if (!hash_contents) {
            hashes = nullptr;
        }
        UploadPrediction prediction{};
        RegionBits rewritten;
        RegionBits unchanged;
        for (const auto& [start, end] : cleaned) {
            copy(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
            if (hashes == nullptr) {
                continue;
            }
            // Hash right after the copy, while the pages are still cached.
            for (const auto& [first, last] : RegionBits(ended, start, end)) {
                for (size_t page = first; page < last;) {
                    std::array<u64, 64> fresh;
                    const size_t count = std::min(last - page, fresh.size());
                    hash_pages(cpu_addr + page * TRACKER_BYTES_PER_PAGE, count, fresh.data());
                    for (size_t i = 0; i < count; ++i, ++page) {
                        if (ahead.Get(page) && hashes->valid.Get(page)) {
                            if (hashes->hash[page] != fresh[i]) {
                                rewritten.Set(page);
                                ++prediction.rewritten;
                            } else {
                                unchanged.Set(page);
                                ++prediction.unchanged;
                            }
                        }
                        hashes->hash[page] = fresh[i];
                    }
                    prediction.hashed += count;
                }
            }
        }
        if (hashes != nullptr) {
            hashes->valid = (hashes->valid & ~cleaned) | ended;
        } else if (ContentHashes* stale = content.load(std::memory_order_acquire)) {
            stale->valid &= ~cleaned;
        }
        if (decay_unchanged.load(std::memory_order_relaxed)) {
            EndWriteCycle(cleaned, rewritten, {});
            return prediction;
        }
        // Without decay every released-ahead page keeps its confidence, rewritten or not.
        for (const auto& [first, last] : ahead) {
            prediction.kept += last - first;
        }
        EndWriteCycle(cleaned, rewritten, ahead);
        return prediction;
    }

    /// Renderer, without the lock: allocate the page hashes once an upload asked for them.
    void PrepareContentHashes() {
        if (!wants_content.load(std::memory_order_relaxed) ||
            content.load(std::memory_order_acquire) != nullptr) {
            return;
        }
        auto* fresh = new ContentHashes{};
        ContentHashes* expected = nullptr;
        if (!content.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) {
            delete fresh;
        }
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
                EndWriteCycle(mask, {}, {});
                if (ContentHashes* hashes = content.load(std::memory_order_acquire)) {
                    hashes->valid &= ~mask;
                }
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

    static constexpr size_t ReleaseWindow =
        std::min<size_t>(WRITE_FAULT_WINDOW_PAGES, NUM_PAGES_PER_REGION);

    /**
     * Streaming writers fill buffers front to back: release the confident pages of
     * [from, window_end), skipping pages already released and stopping at the first page that is
     * GPU modified or (unless predicting across gaps) not expected to be written. `reached_end`
     * tells whether the scan ran to window_end. Returns the number of pages released.
     */
    size_t ReleaseAhead(size_t from, size_t window_end, bool& reached_end) {
        const bool across_gaps = predict_across_gaps.load(std::memory_order_relaxed);
        const RegionBits confident = confidence_lo | confidence_hi;
        size_t predicted = 0;
        reached_end = false;
        for (size_t page = from; page < window_end; ++page) {
            if (gpu.Get(page)) {
                return predicted;
            }
            if (cpu.Get(page)) {
                continue;
            }
            if (!confident.Get(page)) {
                if (across_gaps) {
                    continue;
                }
                return predicted;
            }
            released_ahead.Set(page);
            cpu.Set(page);
            ++predicted;
        }
        reached_end = true;
        return predicted;
    }

    /**
     * An upload cleans `cleaned` and re-watches it, ending those pages' write cycle. The 2-bit
     * write confidence is set to 3 by a real fault and by `rewritten`, released-ahead pages
     * whose contents changed since their previous upload (only known when contents are hashed,
     * see CleanForUpload). Otherwise a released page gives no evidence of whether the CPU wrote
     * it, so its confidence decays by one per cycle spent released ahead, and it drops to 0 when
     * the page was dirty for another reason. Without hashes a page the CPU keeps rewriting
     * faults once every four cycles; one it stopped writing is uploaded redundantly at most
     * three more times. Pages of `kept` (released ahead and uploaded, when released-ahead pages
     * do not decay) keep their confidence.
     */
    void EndWriteCycle(const RegionBits& cleaned, const RegionBits& rewritten,
                       const RegionBits& kept) {
        const RegionBits changed = cleaned & ~kept;
        const RegionBits refreshed = (written & changed) | rewritten;
        const RegionBits ahead = released_ahead & changed & ~refreshed;
        const RegionBits lo = confidence_lo, hi = confidence_hi;
        confidence_lo = (lo & ~changed) | refreshed | (ahead & hi & ~lo);
        confidence_hi = (hi & ~changed) | refreshed | (ahead & hi & lo);
        written &= ~cleaned;
        released_ahead &= ~cleaned;
    }

    /// Contents hash of each page at its last upload, valid for pages whose write cycle ended
    /// with a fault or a release ahead (see CleanForUpload).
    struct ContentHashes {
        std::array<u64, NUM_PAGES_PER_REGION> hash{};
        RegionBits valid;
    };

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
    std::atomic<ContentHashes*> content{}; // allocated by PrepareContentHashes
    std::atomic<bool> wants_content{};     // an upload had pages to hash but no storage
};

} // namespace VideoCore
