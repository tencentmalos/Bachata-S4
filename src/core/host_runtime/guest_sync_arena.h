// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include "common/types.h"
#include "core/host_runtime/guest_sync_abi.h"

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

// Grow by mapped windows, not one mapping/HLE-only block per overflow. The
// initial window keeps the old placement and two-load guest test. Extensions
// publish immutable range nodes through a RW directory. All memory is session
// owned: even destroyed handles keep a unique address until owners have joined.
// Allocation and BindDirectory are serialized by the owning synchronization domain.
class GuestSyncWindows {
public:
    using AllocateRegion = std::function<u64(u64)>; // zeroed mapping; 0 means ENOMEM
    GuestSyncWindows(AllocateRegion allocate, u64 base, u64 limit,
                     u64 preferred_size = SHAD_SYNC_ARENA_WINDOW_SIZE)
        : allocate(std::move(allocate)), next(base), end(limit),
          preferred_size(std::max(MinimumSize, preferred_size / GuestSyncArena::BlockSize *
                                                  GuestSyncArena::BlockSize)) {}

    void BindDirectory(u64* slot) {
        head_slot = slot;
        std::atomic_ref<u64>(*head_slot).store(head, std::memory_order_release);
    }

    u64 AllocateBlock() {
        if (next == end) {
            // Try smaller windows when the guest VA space is fragmented. A later
            // growth retries the preferred size; a transient allocation failure
            // must not permanently disable the fast path or consume an address.
            u64 size = preferred_size;
            u64 base{};
            for (;;) {
                base = allocate(size);
                if (base) break;
                if (size == MinimumSize) return 0;
                size = std::max(MinimumSize, (size / 2) / GuestSyncArena::BlockSize *
                                               GuestSyncArena::BlockSize);
            }
            auto* node = reinterpret_cast<ShadSyncArenaRange*>(base);
            *node = {head, base + GuestSyncArena::BlockSize, base + size};
            head = base;
            if (head_slot)
                std::atomic_ref<u64>(*head_slot).store(head, std::memory_order_release);
            next = node->base;
            end = node->limit;
        }
        const auto block = next;
        next += GuestSyncArena::BlockSize;
        return block;
    }

private:
    static constexpr u64 MinimumSize = 2 * GuestSyncArena::BlockSize; // node + objects
    AllocateRegion allocate;
    u64 next{}, end{}, head{};
    u64* head_slot{};
    u64 preferred_size;
};
} // namespace Core::HostRuntime
