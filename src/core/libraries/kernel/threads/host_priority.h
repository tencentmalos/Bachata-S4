// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

/// Host scheduling weight (a Common::SetThreadNice offset) for a guest thread's Orbis scheduling.
///
/// Orbis uses FreeBSD policy numbers (1 FIFO, 2 OTHER, 3 RR) and priorities where a lower
/// number runs first: FIFO/RR 256 (highest) .. 767 (lowest, default 700), OTHER 768 .. 959.
/// The console schedules them strictly; the host stays time-sharing and only weights the CPU
/// share, in a deliberately narrow band: the most favoured guest threads get the same weight as
/// the emulator's own frame-critical threads (ThreadPriority::High, nice -4), threads at the
/// console default stay where the process started, and lower-priority guest threads yield to both.
/// Shared by the desktop kernel HLE and the Android host runtime.
constexpr int GuestPriorityToHostNice(s32 policy, s32 priority) {
    if (policy == 1 || policy == 3) {
        if (priority < 256 || priority > 767) {
            return 0;
        }
        if (priority < 448) {
            return -4;
        }
        if (priority < 640) {
            return -2;
        }
        if (priority < 704) {
            return 0;
        }
        return 2;
    }
    if (policy == 2) {
        if (priority < 768 || priority > 959) {
            return 0;
        }
        return priority < 864 ? 2 : 4;
    }
    return 0;
}

static_assert(GuestPriorityToHostNice(1, 256) == -4);
static_assert(GuestPriorityToHostNice(1, 447) == -4);
static_assert(GuestPriorityToHostNice(1, 448) == -2);
static_assert(GuestPriorityToHostNice(1, 700) == 0);
static_assert(GuestPriorityToHostNice(3, 704) == 2);
static_assert(GuestPriorityToHostNice(1, 767) == 2);
static_assert(GuestPriorityToHostNice(2, 768) == 2);
static_assert(GuestPriorityToHostNice(2, 959) == 4);
static_assert(GuestPriorityToHostNice(1, 1000) == 0);
static_assert(GuestPriorityToHostNice(0, 256) == 0);

} // namespace Libraries::Kernel
