// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises real MemoryManager lock ordering against owned guest range retirement.
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include "core/guest_cpu/api/address_space.h"
#include "core/memory.h"
using namespace Core;
using namespace Core::GuestCpu;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
struct Backend final : GuestMemoryBackend {
    std::unique_ptr<GuestAddressSpace> space;
    VAddr base{};
    bool prepare_ranges{true};
    std::array<u8, 0x40000> backing{};
    Backend() {
        auto made = GuestAddressSpace::Create({.reservation_size = 0x100000});
        if (!made)
            throw std::runtime_error("reservation");
        space = std::move(made).Value();
        base = (space->ReservationBase().value + 0x3fff) & ~0x3fffULL;
    }
    static void Require(Status status) {
        if (!status)
            throw std::runtime_error(status.GetError().detail);
    }
    u8* BackingBase() const override {
        return const_cast<u8*>(backing.data());
    }
    boost::icl::interval_set<VAddr> UsableRegions() const override {
        boost::icl::interval_set<VAddr> result;
        result.add(boost::icl::interval<VAddr>::right_open(base, base + 0x80000));
        return result;
    }
    bool OwnsRange(VAddr a, u64 n) const override {
        return space->OwnsRange({{a}, n});
    }
    std::unique_ptr<MappingPreparation> PrepareMapping(std::span<const MappingRange> ranges,
                                                       bool executable) override {
        if (ranges.empty() || !prepare_ranges)
            return {};
        struct Prepared final : MappingPreparation {
            std::unique_ptr<GuestAddressSpace::DataRetirement> retirement;
        };
        std::vector<GuestRange> input;
        for (auto [a, n] : ranges)
            input.push_back({{a}, n});
        auto result = space->PrepareDataMapping(input, executable);
        if (!result)
            throw std::runtime_error(result.GetError().detail);
        auto prepared = std::make_unique<Prepared>();
        prepared->retirement = std::move(result).Value();
        return prepared;
    }
    void* Map(VAddr a, u64 n, PAddr, bool executable) override {
        auto p = GuestPermission::Read | GuestPermission::Write;
        if (executable)
            p = p | GuestPermission::Execute;
        Require(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, {{a}, n}, p));
        return reinterpret_cast<void*>(a);
    }
    void* MapFile(VAddr, u64, u64, u32, uintptr_t, bool) override {
        throw std::runtime_error("unused");
    }
    void Unmap(VAddr a, u64 n) override {
        Require(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Unmap, {{a}, n},
                                         GuestPermission::None));
    }
    void Protect(VAddr a, u64 n, MemoryPermission p) override {
        Require(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Protect, {{a}, n},
                                         static_cast<GuestPermission>(p)));
    }
};
int main(int argc, char**) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Backend backend;
    MemoryManager memory(&backend);
    const auto a = backend.base, unrelated = a + 0x10000;
    auto map = [&](VAddr address, VMAType type = VMAType::File, PAddr physical = PAddr(-1)) {
        void* out{};
        return memory.MapMemory(&out, address, 0x4000, MemoryProt::CpuReadWrite,
                                MemoryMapFlags::Fixed, type, "retirement-test", false, physical);
    };
    CHECK(map(unrelated) == 0);
    auto exercise = [&](auto mutate, unsigned expected_ranges) {
        auto pin = backend.space->AcquireDataSpan({{a}, 0x4000}, true);
        CHECK(bool(pin));
        auto mutation = std::async(std::launch::async, mutate);
        auto deadline = std::chrono::steady_clock::now() + 1s;
        while (backend.space->Counts().retiring_ranges < expected_ranges &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        CHECK(backend.space->Counts().retiring_ranges == expected_ranges);
        CHECK(mutation.wait_for(0ms) == std::future_status::timeout);
        auto query = std::async(std::launch::async, [&] {
            void* start{};
            void* end{};
            u32 prot{};
            return memory.QueryProtection(unrelated, &start, &end, &prot) == 0 &&
                   reinterpret_cast<u64>(start) <= unrelated &&
                   reinterpret_cast<u64>(end) > unrelated;
        });
        // A held I/O span must not serialize an unrelated GPU/address query.
        CHECK(query.wait_for(300ms) == std::future_status::ready);
        std::array<std::byte, 4> bytes{std::byte{42}};
        CHECK(bool(backend.space->WriteData({unrelated}, bytes)));
        if (pin)
            pin.Value() = {};
        CHECK(query.get());
        CHECK(mutation.get() == 0);
        CHECK(backend.space->Counts().retiring_ranges == 0);
    };
    CHECK(map(a) == 0);
    backend.prepare_ranges = argc == 1;
    exercise([&] { return memory.Protect(a, 0x4000, MemoryProt::CpuRead); }, 1);
    if (!backend.prepare_ranges) {
        std::printf("legacy-admission counterexample: %u checks, %u failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    CHECK(memory.Protect(a, 0x4000, MemoryProt::CpuReadWrite) == 0);
    exercise([&] { return map(a); }, 1);
    exercise([&] { return memory.UnmapMemory(a, 0x4000); }, 1);
    CHECK(memory.Allocate(0, 0x40000, 0x4000, 0x4000, 0) == 0);
    CHECK(map(a, VMAType::Direct, 0) == 0);
    CHECK(map(a + 0x8000, VMAType::Direct, 0) == 0);
    exercise([&] { return memory.Free(0, 0x4000, true); }, 2);
    CHECK(!backend.space->ValidateRange({{a}, 0x4000}, GuestPermission::Read));
    CHECK(!backend.space->ValidateRange({{a + 0x8000}, 0x4000}, GuestPermission::Read));
    std::printf("guest_memory_manager_concurrency_tests: %u checks, %u failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
