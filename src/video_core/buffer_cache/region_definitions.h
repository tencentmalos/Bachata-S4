// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "common/bit_array.h"
#include "common/types.h"

namespace VideoCore {

constexpr u64 TRACKER_PAGE_BITS = 12; // 4K pages
constexpr u64 TRACKER_BYTES_PER_PAGE = 1ULL << TRACKER_PAGE_BITS;

#if defined(__ANDROID__)
// One bitmap word (64 guest pages) per lock. A GPU upload must not stop CPU
// writers throughout an otherwise unrelated 4 MiB guest heap region.
constexpr u64 TRACKER_HIGHER_PAGE_BITS = 18; // 256 KiB
#else
constexpr u64 TRACKER_HIGHER_PAGE_BITS = 22; // 4 MiB
#endif
constexpr u64 TRACKER_HIGHER_PAGE_SIZE = 1ULL << TRACKER_HIGHER_PAGE_BITS;
constexpr u64 TRACKER_HIGHER_PAGE_MASK = TRACKER_HIGHER_PAGE_SIZE - 1ULL;
constexpr u64 NUM_PAGES_PER_REGION = TRACKER_HIGHER_PAGE_SIZE / TRACKER_BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
};

using RegionBits = Common::BitArray<NUM_PAGES_PER_REGION>;

// A CPU write fault also releases the following watched pages of its 64-page window that the CPU
// is expected to rewrite (streaming data is rewritten every frame). Diagnostic switch.
constexpr u64 WRITE_FAULT_WINDOW_PAGES = 64;
inline std::atomic<bool> predict_write_faults{true};

// Hash the contents of pages whose write cycle ends with a fault or a release ahead, so that a
// page the CPU rewrote while released ahead keeps its write confidence instead of decaying
// (see RegionManager::EndWriteCycle). On by default on Android, where it was measured
// (Bloodborne: write faults 296 -> 173 per frame, +4% FPS); DebugBus `upload_diag watch_content`.
#if defined(__ANDROID__)
inline std::atomic<bool> predict_from_contents{true};
#else
inline std::atomic<bool> predict_from_contents{false};
#endif

// Release ahead every confident page left in the window instead of stopping at the first
// page without write confidence: a writer filling several short runs in one window faults once
// per window, not once per run. Diagnostic switch.
inline std::atomic<bool> predict_across_gaps{false};

// A released-ahead page uploaded without a fault loses one step of write confidence (with
// contents hashed: only when they are unchanged). Off keeps the confidence of every
// released-ahead page: a ring buffer page released early, before the writer reaches it, then
// keeps being released instead of decaying to a fault. A page that stops being written stays
// confident and is uploaded unchanged whenever a fault earlier in its window releases it, at
// most one window per fault. Contents are only hashed while pages decay: without decay the
// result would not change any confidence. Off by default (Bloodborne on Android: write faults
// 165 -> 78 per frame, +2.3% FPS, ~300 more unchanged page uploads per frame); DebugBus
// `upload_diag watch_decay`.
inline std::atomic<bool> decay_unchanged{false};

// A release ahead that reaches the end of its region without meeting a gap or GPU data goes on
// into the first window of the next region: a writer streaming across a region boundary then
// faults once per stream instead of once per region. Diagnostic switch.
inline std::atomic<bool> predict_cross_region{false};

struct ContentPredictionCounters {
    std::atomic<u64> hashed_pages{};    // pages hashed when their write cycle ended
    std::atomic<u64> rewritten_pages{}; // released-ahead pages whose contents changed
    // Write faults by the state of the faulting page: no write confidence; confident with the
    // page before it CPU dirty (released, so this page was watched again or the release stopped
    // here); confident and first of its run.
    std::atomic<u64> fault_cold{}, fault_after_dirty{}, fault_run_start{};
    // Cold faults on pages whose contents were hashed before (their confidence decayed).
    std::atomic<u64> fault_cold_known{};
    // Run-start faults on the first page of a release window (a release never crosses one).
    std::atomic<u64> fault_run_start_window{};
    // Pages released ahead in the next region (predict_cross_region).
    std::atomic<u64> cross_released{};
    // Released-ahead pages uploaded with unchanged contents.
    std::atomic<u64> unchanged_pages{};
    // Released-ahead pages uploaded without a fault that kept their confidence (no decay).
    std::atomic<u64> kept_pages{};
};
inline ContentPredictionCounters content_prediction_counters;

} // namespace VideoCore
