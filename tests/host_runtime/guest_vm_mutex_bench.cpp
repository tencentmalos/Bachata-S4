// SPDX-License-Identifier: GPL-2.0-or-later
// Repeatable host-cost probe. This does not measure guest FPS or FEX execution.
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_sync_arena.h"

using namespace Core::GuestCpu;
// A burst like the thousands of mutex retirements seen in KH3's long frames.
// Setup is outside the measured interval; run the same executable on the target
// device before/after a change. This is host cost, not an end-to-end game frame.
int LifecycleBench() {
    constexpr unsigned count = 6000;
    constexpr auto rw = GuestPermission::Read | GuestPermission::Write;
    for (unsigned round = 0; round < 7; ++round) {
        auto made = GuestAddressSpace::Create({.reservation_size = 128ULL << 20});
        if (!made) throw std::runtime_error(Describe(made.GetError()));
        auto& space = *made.Value();
        const auto base = space.ReservationBase().value;
        // Separate ordinary mappings model a populated game's VM directory.
        for (unsigned i = 0; i < 2048; ++i)
            if (!space.Map({GuestAddress{base + i * 0x8000}, 0x4000}, rw))
                throw std::runtime_error("lifecycle map failed");
        uint64_t next = base + (64ULL << 20);
        Core::HostRuntime::GuestSyncArena arena([&] {
            const auto address = next;
            next += Core::HostRuntime::GuestSyncArena::BlockSize;
            if (!space.Map({GuestAddress{address}, Core::HostRuntime::GuestSyncArena::BlockSize}, rw))
                throw std::runtime_error("lifecycle arena failed");
            return address;
        });
        Core::HostRuntime::GuestMutexDomain locks(space, [&] { return arena.Allocate(); });
        const auto slot = [base](unsigned i) {
            return base + (i % 2048) * 0x8000 + (i / 2048) * 8;
        };
        for (unsigned i = 0; i < count; ++i)
            if (locks.Init(slot(i), 0)) throw std::runtime_error("lifecycle init failed");
        const auto start = std::chrono::steady_clock::now();
        // The permutation avoids making ordered-tree retirement look cheaper
        // merely because every erase happens to remove the first element.
        for (unsigned i = 0; i < count; ++i)
            if (locks.Destroy(slot((i * 1999) % count)))
                throw std::runtime_error("lifecycle destroy failed");
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("{\"round\":%u,\"destroy_calls\":%u,\"destroy_ms\":%.6f}\n", round, count, ms);
    }
    return 0;
}

int LookupScaleBench() {
    const auto page = HostPageSize();
    constexpr auto rw = GuestPermission::Read | GuestPermission::Write;
    // Keep mapped pages + gaps below Android's per-process VMA limit (65530).
    for (const unsigned count : {2048u, 8192u, 16384u}) {
        auto made = GuestAddressSpace::Create({.reservation_size = page * count * 2});
        if (!made) throw std::runtime_error(Describe(made.GetError()));
        auto& space = *made.Value();
        const auto base = space.ReservationBase().value;
        for (unsigned i = 0; i < count; ++i) {
            const auto slot = (i * 997) % count;
            if (!space.MapFreshData({{base + slot * page * 2}, page}, rw))
                throw std::runtime_error("lookup scale mapping failed");
        }
        for (unsigned round = 0; round < 3; ++round) {
            uint32_t key = 0x12345678;
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 200000; ++i) {
                key = key * 1664525u + 1013904223u;
                const auto slot = (key >> 8) % count;
                if (!space.ValidateRange({{base + slot * page * 2}, 8}, rw))
                    throw std::runtime_error("lookup scale validation failed");
            }
            const auto ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("{\"mappings\":%u,\"round\":%u,\"queries\":200000,\"ms\":%.6f}\n", count, round, ms);
            std::fflush(stdout);
        }
    }
    return 0;
}

int main(int argc, char** argv) try {
    if (argc > 1 && std::string_view(argv[1]) == "--lookup-scale") return LookupScaleBench();
    if (argc > 1 && std::string_view(argv[1]) == "--lifecycle") return LifecycleBench();
    auto created = GuestAddressSpace::Create({.reservation_size = 128ULL << 20});
    if (!created) throw std::runtime_error(Describe(created.GetError()));
    auto& space = *created.Value();
    const auto base = space.ReservationBase().value;
    constexpr auto rw = GuestPermission::Read | GuestPermission::Write;
    constexpr uint64_t stride = 0x8000;
    // Interleave insertion order. Optimizing lookup must not reorder mappings.
    for (unsigned parity = 0; parity != 2; ++parity)
        for (unsigned i = parity; i < 2048; i += 2)
            if (!space.Map({GuestAddress{base + i * stride}, 0x4000}, rw))
                throw std::runtime_error("Map failed");
    uint64_t next = base + 2048 * stride;
    Core::HostRuntime::GuestMutexDomain locks(space, [&] {
        const auto addr = next;
        next += stride;
        if (!space.Map({GuestAddress{addr}, 0x4000}, rw))
            throw std::runtime_error("mutex allocation failed");
        return addr;
    });
    std::array<uint64_t, 8> slots{};
    for (unsigned i = 0; i < slots.size(); ++i) {
        slots[i] = base + (2047 - i * 2) * stride;
        if (locks.Init(slots[i], 0)) throw std::runtime_error("mutex init failed");
    }
    using clock = std::chrono::steady_clock;
    for (int round = 0; round < 3; ++round) {
        auto start = clock::now();
        for (unsigned i = 0; i < 200000; ++i) {
            const GuestRange r{GuestAddress{slots[i % slots.size()]}, 8};
            if (!space.ValidateRange(r, rw)) throw std::runtime_error("validation failed");
        }
        const auto validation = std::chrono::duration<double, std::milli>(clock::now() - start).count();
        start = clock::now();
        for (unsigned i = 0; i < 20000; ++i) {
            const auto slot = slots[i % slots.size()];
            if (locks.Lock(slot, 0x1234, false, {}) || locks.Unlock(slot, 0x1234))
                throw std::runtime_error("mutex operation failed");
        }
        const auto mutex = std::chrono::duration<double, std::milli>(clock::now() - start).count();
        std::printf("{\"round\":%d,\"mappings\":2056,\"validate_calls\":200000,"
                    "\"validate_ms\":%.6f,\"mutex_pairs\":20000,\"mutex_ms\":%.6f}\n",
                    round, validation, mutex);
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
