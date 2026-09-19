// SPDX-License-Identifier: GPL-2.0-or-later
// Editable guest C++ entry interception. Original body still runs through FEX.
// The adapter restores all captured registers/FP state and tail-jumps original.
// Keep this callback bounded and return normally; do not call the same entry.
#include "shad_entry.h"
namespace { shad_u64 calls; }
extern "C" void intercept_TMNT_UpdateEnabledObjectSubtree(const ShadGuestEntryContext* entry) {
    // entry->gpr contains opaque machine arguments; entry->rsp points at the
    // unchanged return PC, followed by stack arguments. Recover types before
    // interpreting guest pointers. This snapshot is read-only and short-lived.
    (void)entry;
    const auto count = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    // One first-hit report, then one per 4096 entries. No per-entry host call.
    if (count == 1 || (count & 4095) == 0)
        shad_sdk_counter(43, (shad_i64)count);
}
