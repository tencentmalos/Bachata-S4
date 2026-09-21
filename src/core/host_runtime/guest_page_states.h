// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
#include "common/types.h"

namespace Core::HostRuntime {
// Published chunks remain alive until all guest/fault owners have joined.
// Fault-side lookup is bounded, lock-free, and never allocates.
class GuestPageStates {
public:
    static constexpr u64 AddressLimit = 1ULL << 39;
    static constexpr u64 ChunkBytes = 1ULL << 30;
    static constexpr u64 PagesPerChunk = ChunkBytes / 4096;
    using Chunk = std::array<std::atomic<u8>, PagesPerChunk>;
    static_assert(std::atomic<Chunk*>::is_always_lock_free);
    GuestPageStates() = default;
    GuestPageStates(const GuestPageStates&) = delete;
    ~GuestPageStates() {
        for (auto& chunk : chunks) delete chunk.load(std::memory_order_relaxed);
    }
    void Ensure(u64 address, u64 size) {
        if (!size || address >= AddressLimit || size > AddressLimit - address)
            throw std::out_of_range("guest page states");
        for (u64 i = address / ChunkBytes; i <= (address + size - 1) / ChunkBytes; ++i) {
            if (chunks[i].load(std::memory_order_acquire)) continue;
            auto created = std::make_unique<Chunk>();
            Chunk* expected{};
            if (chunks[i].compare_exchange_strong(expected, created.get(),
                                                  std::memory_order_release,
                                                  std::memory_order_acquire)) created.release();
        }
    }
    std::atomic<u8>& At(u64 page) {
        return (*chunks[page / PagesPerChunk].load(std::memory_order_acquire))[page % PagesPerChunk];
    }
    u8 Load(u64 address) const noexcept {
        if (address >= AddressLimit) return 0;
        const auto* chunk = chunks[address / ChunkBytes].load(std::memory_order_acquire);
        return chunk ? (*chunk)[(address / 4096) % PagesPerChunk].load(std::memory_order_acquire) : 0;
    }
    u64 AllocatedBytes() const noexcept {
        u64 count{};
        for (const auto& chunk : chunks) count += chunk.load(std::memory_order_acquire) != nullptr;
        return count * sizeof(Chunk);
    }
private:
    std::array<std::atomic<Chunk*>, AddressLimit / ChunkBytes> chunks{};
};
} // namespace Core::HostRuntime
