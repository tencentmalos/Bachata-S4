// SPDX-License-Identifier: GPL-2.0-or-later
// Repeatable host-cost probe. This does not measure guest FPS or FEX execution.
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include "core/host_runtime/guest_mutex.h"

using namespace Core::GuestCpu;
int main() try {
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
