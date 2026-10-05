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

// Each region records whether any of its pages is GPU modified, so that asking whether a range is
// GPU modified skips the region lock and the bit scan in regions without such pages (Bloodborne:
// 64% of the queries, mostly reads of per-draw constants). DebugBus `upload_diag gpu_flag on|off`,
// on by default.
inline std::atomic<bool> region_gpu_flag{true};

// Streaming pages (GCN v2 spec 3.5): a page the CPU rewrote in each of its last three write
// cycles stops being write protected by the buffer cache, saving a write fault and two
// protection changes per cycle. It stays CPU modified; every upload that covers it hashes its
// contents and copies it only when they changed since its last copy, so what the GPU reads is
// what guest memory held at the binding, as with write tracking. A hash costs about as much as
// a protection change, so only pages bound at most StreamMaxBinds times in their last write
// cycle are promoted, through uploads of read-only bindings of at most StreamMaxBinding bytes.
// A page is watched again (demoted) when a binding writes it, when it is unmarked or cleaned
// explicitly, when StreamDemoteChecks uploads in a row found it unchanged (the CPU stopped
// writing it, or it is bound more often now), and when the switch is turned off.
// DebugBus `upload_diag watch_stream on|off [cycles]`, SHADPS4_WATCH_STREAM.
inline std::atomic<bool> stream_pages{false};
// Consecutive write cycles ended by a CPU write that promote a page (1–3). Fewer promote more
// pages: a short GPU replay exercises the copy path with 1 (SHADPS4_WATCH_STREAM_CYCLES).
inline std::atomic<u32> stream_promote_cycles{3};
constexpr u64 StreamMaxBinding = 64 * 1024;
constexpr u16 StreamMaxBinds = 4;
// Contents the CPU rewrites unchanged also count as unchanged: a low limit would demote and
// promote such pages again every few frames, each time with three write faults.
constexpr u16 StreamDemoteChecks = 256;
struct StreamPageCounters {
    std::atomic<u64> promoted{}; ///< Pages that stopped being write protected.
    std::atomic<u64> demoted{};  ///< Pages watched again.
    std::atomic<u64> checked{};  ///< Streaming page contents hashed by an upload.
    std::atomic<u64> copied{};   ///< Streaming pages copied (contents changed, or promoted).
    std::atomic<u64> busy{};     ///< Promotions refused: bound too often (StreamMaxBinds).
};
inline StreamPageCounters stream_page_counters;
/// How an upload treats streaming pages.
enum class StreamUpload : u8 {
    Promote, ///< Small read-only binding: checks streaming pages, promotes pages.
    Keep,    ///< Other read-only binding: checks streaming pages.
    Demote,  ///< Binding the GPU writes: demotes the streaming pages of its range.
};

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
