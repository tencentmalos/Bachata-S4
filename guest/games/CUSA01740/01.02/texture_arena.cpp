// SPDX-License-Identifier: GPL-2.0-or-later
#include "shad_imports.h"

// This is a game compatibility package. The original allocator still owns the
// allocation, mapping, compaction, GPU fences and retirement of every texture.
extern "C" unsigned int initialize_renderer() {
    const auto before = *video_arena_bytes;
    if (before == 0x50000000ULL) {
        *video_arena_bytes = 0x90000000ULL;
    }
    shad_sdk_log(11, before);
    shad_sdk_log(12, *video_arena_bytes);
    const auto result = original_initialize_renderer();
    shad_sdk_log(13, result);
    shad_sdk_log(14, *vram_capacity);
    return result;
}

static shad_u64 calls, failures;

extern "C" shad_u64 probe_allocate(shad_u64* slot, unsigned int size, shad_u64 key,
                                     unsigned int flags) {
    const auto old = *slot;
    const unsigned int used = *vram_used, capacity = *vram_capacity;
    const auto result = original_allocate(slot, size, key, flags);
    const auto n = __atomic_add_fetch(&calls, 1ULL, __ATOMIC_RELAXED);
    const auto failure = result ? 0 : __atomic_add_fetch(&failures, 1ULL, __ATOMIC_RELAXED);
    // Keep evidence bounded even if an unexpected area exceeds the larger arena.
    if (n == 1 || (failure && failure <= 64)) {
        shad_sdk_log(1, failure);
        shad_sdk_log(2, n);
        shad_sdk_log(3, size);
        shad_sdk_log(4, used);
        shad_sdk_log(5, capacity);
        shad_sdk_log(6, reinterpret_cast<shad_u64>(slot));
        shad_sdk_log(7, old);
        shad_sdk_log(8, *slot);
        shad_sdk_log(9, key);
        shad_sdk_log(10, result);
    }
    return result;
}
