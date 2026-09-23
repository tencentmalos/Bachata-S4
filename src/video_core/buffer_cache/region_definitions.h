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

} // namespace VideoCore
