// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include "common/types.h"

namespace Vulkan {

// Lock-free render pass tile traffic counters, incremented by the guest command
// scheduler per pass instance and read by the StatusLayer / gpu_memory as deltas.
// A "load" is an attachment opened with LOAD_OP_LOAD (its contents are read into
// tile memory), a "clear" one opened with LOAD_OP_CLEAR, and every attachment is
// stored. Pixels are width x height x layers per attachment; the driver may still
// render a pass in sysmem mode, so these count what we ask for, not GMEM traffic.
struct RenderPassStats {
    std::atomic<u64> passes{}, loads{}, clears{}, stores{};
    std::atomic<u64> load_pixels{}, store_pixels{};
    // Passes by the number of draws recorded inside them.
    std::atomic<u64> empty{}, single{}, few{}, many{};
    // Operations placed before the open pass instead of breaking it, refusals because
    // of a dependency on the pass so far or because no pass was held, and hoists cut by
    // a submission or raw command buffer access (split around the pass end).
    std::atomic<u64> hoisted{}, hoist_conflicts{}, hoist_unavailable{}, hoist_interrupted{};
    std::atomic<bool> hoist_off{};

    struct Snapshot {
        u64 passes, loads, clears, stores, load_pixels, store_pixels, empty, single, few, many;
        u64 hoisted, hoist_conflicts, hoist_unavailable, hoist_interrupted;
    };
    Snapshot Read() const noexcept {
        constexpr auto o = std::memory_order_relaxed;
        return {passes.load(o),      loads.load(o),        clears.load(o), stores.load(o),
                load_pixels.load(o), store_pixels.load(o), empty.load(o),  single.load(o),
                few.load(o),         many.load(o),         hoisted.load(o), hoist_conflicts.load(o),
                hoist_unavailable.load(o), hoist_interrupted.load(o)};
    }
};

inline RenderPassStats render_pass_stats;

} // namespace Vulkan
