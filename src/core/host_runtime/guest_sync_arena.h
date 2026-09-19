// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include "common/types.h"

namespace Core::HostRuntime {
// Session-owned ABI prefixes; addresses stay unique until Session teardown.
// Caller serializes allocation, never lock/unlock or condition waits. A block
// allocator returns a fresh zeroed 16 KiB guest mapping, or zero on exhaustion.
class GuestSyncArena {
public:
    static constexpr u64 BlockSize = 0x4000;
    static constexpr u64 ObjectSize = 0x100;
    explicit GuestSyncArena(std::function<u64()> allocate_block)
        : allocate_block(std::move(allocate_block)) {}
    u64 Allocate() {
        if (next == end) {
            const auto block = allocate_block();
            if (!block) return 0;
            next = block;
            end = block + BlockSize;
        }
        const auto address = next;
        next += ObjectSize;
        return address;
    }
private:
    std::function<u64()> allocate_block;
    u64 next{}, end{};
};
} // namespace Core::HostRuntime
