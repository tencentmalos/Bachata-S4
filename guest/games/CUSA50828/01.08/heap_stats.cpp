// SPDX-License-Identifier: GPL-2.0-or-later
// Exact TMNT callsite optimization. The real guest mspace allocator, free,
// heap lock and public statistics API remain intact. This runs inside FEX.
#include "shad_imports.h"
namespace {
constexpr bool SkipDiscardedStats = true;
alignas(64) shad_u64 allocation_queries;
alignas(64) shad_u64 release_queries;
alignas(64) shad_u64 forwarded_queries;
void report(shad_u64& count, unsigned int id) {
    const auto n = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
    // Same diagnostic sampling in original-query and optimized builds.
    if (n == 1 || (n & 4095) == 0) shad_sdk_counter(id, n);
}
}
extern "C" __attribute__((noinline)) int patch_heap_stats_fast(void* heap, void* output) {
    const auto caller = reinterpret_cast<shad_u64>(__builtin_return_address(0));
    // Derive the loaded module base from its checked data binding (ASLR-safe).
    const auto module = reinterpret_cast<shad_u64>(tmnt_mspace_slot) - 0x1e16ac8;
    const bool allocated = caller == module + 0xc6362;
    const bool released = caller == module + 0xc63cd;
    // These two original wrappers have just called real mspace malloc/free.
    // Their correctly versioned 40-byte stack result is never read. All other
    // callsites, other heaps and argument shapes retain the original function.
    if ((allocated || released) && heap &&
        reinterpret_cast<shad_u64>(heap) == *tmnt_mspace_slot && output &&
        *static_cast<const unsigned int*>(output) == 0x00010028) {
        report(allocated ? allocation_queries : release_queries, allocated ? 50 : 51);
        if (SkipDiscardedStats) return 0;
    } else {
        report(forwarded_queries, 52);
    }
    return original_heap_stats_fast(heap, output);
}
