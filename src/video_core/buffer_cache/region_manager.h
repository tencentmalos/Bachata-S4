// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <limits>

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
        gpu_pages.store(false, std::memory_order_release);
        writeable.Fill();
        readable.Fill();
        written.Clear();
        released_ahead.Clear();
        confidence_lo.Clear();
        confidence_hi.Clear();
        DemoteStreaming(streaming); // a pooled manager reused: keeps the counters exact
        stream_runs_lo.Clear();
        stream_runs_hi.Clear();
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

        // A streaming page marked CPU modified already is; its contents are checked at its next
        // upload.
        if (!(type == Type::CPU && enable) && streaming.Any()) {
            RegionBits range;
            range.SetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                // No longer CPU modified: watched again below.
                DemoteStreaming(streaming & range);
            } else if constexpr (enable) {
                // GPU written (uploads for GPU-written bindings demote first): the GPU copy is
                // current, the page is watched again.
                const RegionBits demoted = streaming & range;
                if (demoted.Any()) {
                    DemoteStreaming(demoted);
                    cpu &= ~demoted;
                    UpdateProtection<true, false>();
                }
            }
        }
        RegionBits& bits = GetRegionBits<type>();
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::GPU) {
            gpu_pages.store(enable || gpu.Any(), std::memory_order_release);
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
                      : std::min<size_t>(Common::DivCeil(end_page, ReleaseWindow) * ReleaseWindow,
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
     * Streaming pages (see stream_pages) stay CPU modified and unwatched; the ones whose
     * contents ForEachPendingUpload found changed are copied too. With StreamUpload::Promote,
     * pages whose last write cycles all ended with a CPU write become streaming. Must follow
     * ForEachPendingUpload of the same range under the same lock: the copied runs are exactly the
     * ones it reported.
     */
    UploadPrediction CleanForUpload(VAddr query_cpu_range, s64 size, bool hash_contents,
                                    StreamUpload mode, auto&& copy, auto&& hash_pages) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return {};
        }
        const RegionBits cleaned(cpu, start_page, end_page);
        // Streaming pages imply the hashes (see the promotion below).
        ContentHashes* stream_hashes = content.load(std::memory_order_acquire);
        if (stream_hashes != nullptr && stream_pages.load(std::memory_order_relaxed)) {
            CountBindings(*stream_hashes, start_page, end_page);
        }
        const RegionBits checked = cleaned & streaming;
        const RegionBits changed =
            checked.Any() ? checked & stream_hashes->changed : RegionBits{};
        RegionBits normal = cleaned & ~streaming;
        RegionBits promoted;
        if (mode == StreamUpload::Promote && normal.Any() &&
            stream_pages.load(std::memory_order_relaxed)) {
            // Count each page's consecutive write cycles that ended with a CPU write (2 bits,
            // saturating); a cycle that ended without one starts the count again.
            const RegionBits ended_by_cpu = (written | released_ahead) & normal;
            const RegionBits lo = stream_runs_lo, hi = stream_runs_hi;
            stream_runs_lo = (lo & ~normal) | (ended_by_cpu & (~lo | hi));
            stream_runs_hi = (hi & ~normal) | (ended_by_cpu & (hi | lo));
            // Counts 1, 2, 3 are (lo, hi) = (1, 0), (0, 1), (1, 1).
            const u32 cycles = stream_promote_cycles.load(std::memory_order_relaxed);
            const RegionBits reached = cycles >= 3   ? stream_runs_lo & stream_runs_hi
                                       : cycles == 2 ? stream_runs_hi
                                                     : stream_runs_lo | stream_runs_hi;
            promoted = ended_by_cpu & reached & ~gpu;
            if (stream_hashes == nullptr && ended_by_cpu.Any()) {
                // A streaming page needs the hash of its last copy, and its bindings are
                // counted there: allocated before the next upload (PrepareContentHashes).
                wants_content.store(true, std::memory_order_relaxed);
                promoted = {};
            }
            if (promoted.Any()) {
                promoted = DropBusyPages(*stream_hashes, promoted);
            }
            if (promoted.Any()) {
                streaming |= promoted;
                normal &= ~promoted;
                // Hashed before the copy: a write in between is seen by the next check.
                HashStreamingPages(*stream_hashes, promoted, hash_pages);
                stream_page_counters.promoted.fetch_add(CountPages(promoted),
                                                        std::memory_order_relaxed);
            }
        }
        cpu &= ~normal;
        UpdateProtection<true, false>();
        // Copied: the cleaned pages, the promoted ones and the streaming pages that changed.
        const RegionBits copied = normal | promoted | changed;

        const RegionBits ended = (written | released_ahead) & normal;
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
        for (const auto& [start, end] : copied) {
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
        // Only once copied: a streaming page keeps the hash of a copy the GPU holds.
        if (checked.Any() || promoted.Any()) {
            CommitStreamingPages(*stream_hashes, checked, changed | promoted);
        }
        if (stream_hashes != nullptr) {
            for (const auto& [first, last] : copied) {
                std::fill(stream_hashes->binds.begin() + first,
                          stream_hashes->binds.begin() + last, 0);
            }
        }
        if (hashes != nullptr) {
            hashes->valid = (hashes->valid & ~normal) | ended;
        } else if (ContentHashes* stale = content.load(std::memory_order_acquire)) {
            stale->valid &= ~normal;
        }
        if (decay_unchanged.load(std::memory_order_relaxed)) {
            EndWriteCycle(normal, rewritten, {});
            return prediction;
        }
        // Without decay every released-ahead page keeps its confidence, rewritten or not.
        for (const auto& [first, last] : ahead) {
            prediction.kept += last - first;
        }
        EndWriteCycle(normal, rewritten, ahead);
        return prediction;
    }

    /**
     * SnapshotForUpload, before CleanForUpload of the same range under the same lock: call
     * func(address, size) for each run of pages the upload copies. These are the CPU modified
     * pages, less the streaming pages whose contents did not change since their last copy
     * (hashed here by hash_pages(address, count, hashes)). Streaming pages are demoted first when
     * the GPU writes the binding (all of them when the switch is off): they are CPU modified, so
     * the upload cleans and copies them.
     */
    void ForEachPendingUpload(VAddr query_cpu_range, s64 size, StreamUpload mode,
                              auto&& hash_pages, auto&& func) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }
        if (streaming.Any()) {
            if (!stream_pages.load(std::memory_order_relaxed)) {
                DemoteStreaming(streaming);
            } else if (mode == StreamUpload::Demote) {
                DemoteStreaming(RegionBits(streaming, start_page, end_page));
            }
        }
        RegionBits pending(cpu, start_page, end_page);
        if (const RegionBits check = pending & streaming; check.Any()) {
            ContentHashes& hashes = *content.load(std::memory_order_acquire);
            hashes.changed &= ~check;
            for (const auto& [first, last] : check) {
                for (size_t page = first; page < last;) {
                    const size_t count = std::min<size_t>(last - page, 64);
                    hash_pages(cpu_addr + page * TRACKER_BYTES_PER_PAGE, count,
                               hashes.pending.data() + page);
                    for (const size_t end = page + count; page < end; ++page) {
                        if (!hashes.valid.Get(page) || hashes.hash[page] != hashes.pending[page]) {
                            hashes.changed.Set(page);
                        }
                    }
                }
            }
            pending &= ~(check & ~hashes.changed);
        }
        for (const auto& [start, end] : pending) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
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
            if constexpr (type == Type::CPU) {
                // Cleaned pages are watched again.
                DemoteStreaming(streaming & mask);
            }
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::GPU) {
                gpu_pages.store(gpu.Any(), std::memory_order_release);
            }
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

    /// False when no page of the region is GPU modified. Readable without the lock: every
    /// change of the GPU bits stores it under the lock.
    [[nodiscard]] bool MayHaveGpuPages() const noexcept {
        return gpu_pages.load(std::memory_order_acquire);
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

    /// Streaming pages watched again from their next upload: they stay CPU modified until then.
    /// Takes a copy: `pages` may be `streaming` itself.
    void DemoteStreaming(const RegionBits pages) {
        if (pages.None()) {
            return;
        }
        streaming &= ~pages;
        stream_runs_lo &= ~pages;
        stream_runs_hi &= ~pages;
        stream_page_counters.demoted.fetch_add(CountPages(pages), std::memory_order_relaxed);
    }

    static u64 CountPages(const RegionBits& pages) {
        u64 count = 0;
        for (const auto& [first, last] : pages) {
            count += last - first;
        }
        return count;
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
    /// Streaming pages (see stream_pages) use the same hashes for the contents of their last copy,
    /// plus the hashes an upload computed (`pending`, ForEachPendingUpload), which pages changed,
    /// and the bindings of each page since its last copy (counted while streaming pages are on).
    struct ContentHashes {
        std::array<u64, NUM_PAGES_PER_REGION> hash{};
        RegionBits valid;
        std::array<u64, NUM_PAGES_PER_REGION> pending{};
        RegionBits changed;
        std::array<u16, NUM_PAGES_PER_REGION> binds{};
    };

    void CountBindings(ContentHashes& hashes, size_t start_page, size_t end_page) {
        for (size_t page = start_page; page < end_page; ++page) {
            u16& binds = hashes.binds[page];
            binds += binds != std::numeric_limits<u16>::max();
        }
    }

    /// Promotion candidates bound more than StreamMaxBinds times in the cycle that ends now: each
    /// binding of a streaming page costs a hash, more than write tracking costs for such a page.
    RegionBits DropBusyPages(const ContentHashes& hashes, RegionBits candidates) {
        u64 busy = 0;
        for (const auto& [first, last] : RegionBits(candidates)) {
            for (size_t page = first; page < last; ++page) {
                if (hashes.binds[page] > StreamMaxBinds) {
                    candidates.Unset(page);
                    ++busy;
                }
            }
        }
        if (busy != 0) {
            stream_page_counters.busy.fetch_add(busy, std::memory_order_relaxed);
        }
        return candidates;
    }

    /// The hashes of newly promoted streaming pages, before their copy.
    void HashStreamingPages(ContentHashes& hashes, const RegionBits& pages, auto&& hash_pages) {
        for (const auto& [first, last] : pages) {
            for (size_t page = first; page < last;) {
                const size_t count = std::min<size_t>(last - page, 64);
                hash_pages(cpu_addr + page * TRACKER_BYTES_PER_PAGE, count,
                           hashes.pending.data() + page);
                page += count;
            }
        }
    }

    /**
     * An upload checked the streaming pages `checked` and copies `copied` (the changed ones and
     * the newly promoted ones): those keep the hash of their copy (ForEachPendingUpload or
     * HashStreamingPages). A page unchanged over StreamDemoteChecks checks in a row is demoted
     * (the CPU no longer rewrites it, or it is bound more often than it changes): it is cleaned
     * and watched at its next upload.
     */
    void CommitStreamingPages(ContentHashes& hashes, const RegionBits& checked,
                              const RegionBits& copied) {
        RegionBits stale;
        for (const auto& [first, last] : checked | copied) {
            for (size_t page = first; page < last; ++page) {
                if (copied.Get(page)) {
                    hashes.hash[page] = hashes.pending[page];
                } else if (hashes.binds[page] >= StreamDemoteChecks) {
                    stale.Set(page);
                }
            }
        }
        hashes.valid |= copied;
        // Streaming pages have no write cycles.
        written &= ~(checked | copied);
        released_ahead &= ~(checked | copied);
        auto& counters = stream_page_counters;
        counters.checked.fetch_add(CountPages(checked), std::memory_order_relaxed);
        counters.copied.fetch_add(CountPages(copied), std::memory_order_relaxed);
        DemoteStreaming(stale);
    }

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    std::atomic<bool> gpu_pages{}; ///< gpu.Any(), see MayHaveGpuPages.
    RegionBits writeable;
    RegionBits readable;
    RegionBits written;        // CPU write faults since the upload that last cleaned the page
    RegionBits released_ahead; // released by MarkWriteFault prediction in this cycle
    RegionBits confidence_lo;  // 2-bit write confidence (see EndWriteCycle)
    RegionBits confidence_hi;
    // Streaming pages (see stream_pages): CPU modified and not watched by the buffer cache.
    RegionBits streaming;
    RegionBits stream_runs_lo; // 2-bit count of consecutive write cycles ended by a CPU write
    RegionBits stream_runs_hi;
    std::atomic<ContentHashes*> content{}; // allocated by PrepareContentHashes
    std::atomic<bool> wants_content{};     // an upload had pages to hash but no storage
};

} // namespace VideoCore
