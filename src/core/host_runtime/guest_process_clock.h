// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include "common/types.h"
#include "core/host_runtime/guest_sync_abi.h"

namespace Core::HostRuntime {
// Process elapsed time excludes app suspension. The freestanding guest fast path reads the
// same wire state, so switching between leaf calls and HLE never changes the clock domain.
class GuestProcessClock {
public:
    explicit GuestProcessClock(u64 origin) : local{0, origin, 0} {}

    // Preparation only, before this session admits any guest owner.
    void Bind(ShadProcessClock* shared) {
        *shared = local;
        state = shared;
    }

    template <typename ReadNow>
    u64 Read(ReadNow now) const {
        for (;;) {
            const auto sequence = Load(state->sequence);
            if (sequence & 1) continue;
            const auto origin = Load(state->origin);
            const auto frozen = Load(state->frozen);
            const auto end = frozen ? frozen : now();
            if (sequence == Load(state->sequence)) return end - origin;
        }
    }

    template <typename ReadNow>
    void SetPaused(bool paused, ReadNow now) {
        std::lock_guard lock(mutex);
        const auto frozen = Load(state->frozen);
        if (paused == (frozen != 0)) return;
        // All fields are atomic, including sequence validation, on host and guest. The writer
        // is short and does no HLE/VM/device work while the sequence is odd.
        auto sequence = std::atomic_ref{state->sequence};
        sequence.fetch_add(1);
        if (paused) Store(state->frozen, now());
        else {
            Store(state->origin, Load(state->origin) + now() - frozen);
            Store(state->frozen, 0);
        }
        sequence.fetch_add(1);
    }

private:
    static u64 Load(u64& value) { return std::atomic_ref{value}.load(); }
    static void Store(u64& value, u64 next) { std::atomic_ref{value}.store(next); }
    ShadProcessClock local;
    ShadProcessClock* state{&local};
    std::mutex mutex;
};
static_assert(sizeof(ShadProcessClock) == 24 && alignof(ShadProcessClock) >= 8);
} // namespace Core::HostRuntime
