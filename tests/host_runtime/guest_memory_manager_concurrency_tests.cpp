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
#include "core/host_runtime/guest_direct_memory_hle.h"
#include "core/file_sys/ifile.h"
#include <cstring>
#include <sstream>
#include <sys/mman.h>
#include "core/host_runtime/guest_reservation.h"
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
        auto made = GuestAddressSpace::Create({.reservation_size = 0x1000000});
        if (!made)
            throw std::runtime_error("reservation");
        space = std::move(made).Value();
        base = (space->ReservationBase().value + 0x1fffff) & ~0x1fffffULL;
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
        result.add(boost::icl::interval<VAddr>::right_open(base, base + 0x800000));
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
    void* MapFile(VAddr a, u64 n, u64 offset, u32, uintptr_t handle, bool) override {
        if (offset != u64(-1) || handle != uintptr_t(-1))
            throw std::runtime_error("unexpected native mapping");
        return Map(a, n, 0, false);
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
struct CopySource final : FileSys::IFile {
    std::promise<void> entered, resume;
    bool short_read{};
    s64 ReadAt(void* data, u64 size, u64 offset) override {
        entered.set_value(); resume.get_future().wait();
        if (short_read) return -1;
        if (offset != 0 || size != 4) throw std::runtime_error("copy range");
        std::memcpy(data, "DATA", 4); return 4;
    }
    s64 Read(void*, u64) override { return -1; }
    s64 Write(const void*, u64) override { return -1; }
    bool Seek(s64, Common::FS::SeekOrigin) override { return false; }
    u64 Tell() const override { return 0; }
    u64 Size() const override { return 4; }
    bool Flush() override { return true; }
    bool IsOpen() const override { return true; }
    FileSys::MmapPolicy GetMmapPolicy() const override { return FileSys::MmapPolicy::Copy; }
};
int main(int argc, char**) {
    {
        const std::array<GuestRange, 2> proposed{{{{0x400000}, 0xfc00000},
                                                {{0x100000000}, 0x100000000}}};
        std::istringstream maps("02000000-12000000 rw-p 0 00:00 0 [anon:dalvik-main space]\n"
                                "180000000-180004000 rw-p 0 00:00 0\n"
                                "180004000-180004000 r-xp 0 00:00 0\n");
        auto planned = Core::HostRuntime::ExcludeHostMappings(proposed, maps);
        CHECK(planned && planned.Value().size() == 3);
        CHECK(planned && planned.Value()[0].base.value == 0x400000 &&
              planned.Value()[0].End() == 0x2000000);
        CHECK(planned && planned.Value()[1].End() == 0x180000000 &&
              planned.Value()[2].base.value == 0x180004000);
        for (const auto text : {"", "broken\n", "2000-1000 rw-p\n",
                                "1000-3000 rw-p\n2000-4000 rw-p\n"}) {
            std::istringstream bad(text);
            CHECK(!Core::HostRuntime::ExcludeHostMappings(proposed, bad));
        }
        // A host mapping appearing after the snapshot must never be overwritten.
        auto* host = static_cast<u8*>(::mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        CHECK(host != MAP_FAILED);
        if (host != MAP_FAILED) {
            host[0] = 0x5a;
            AddressSpaceConfig cfg;
            cfg.preferred_base = reinterpret_cast<u64>(host);
            cfg.reservation_size = 0x4000;
            cfg.owned_ranges = {{{cfg.preferred_base}, 0x4000}};
            CHECK(!GuestAddressSpace::Create(cfg));
            CHECK(host[0] == 0x5a);
            ::munmap(host, 0x4000);
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Backend backend;
    MemoryManager memory(&backend);
    Memory::Binding memory_binding(memory);
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
    for (const bool bad : {false, true}) {
        CopySource source; source.short_read = bad;
        NativeFileMapping file{uintptr_t(-1), false, &source};
        void* mapped{};
        auto pending = std::async(std::launch::async, [&] {
            return memory.MapFile(&mapped, a, 0x4000, MemoryProt::CpuReadWrite,
                                  MemoryMapFlags::Fixed, 123, 0, &file);
        });
        CHECK(source.entered.get_future().wait_for(1s) == std::future_status::ready);
        auto query = std::async(std::launch::async, [&] {
            void* start{}; void* end{}; u32 prot{};
            return memory.QueryProtection(unrelated, &start, &end, &prot);
        });
        CHECK(query.wait_for(300ms) == std::future_status::ready);
        CHECK(backend.space->Counts().retiring_ranges == 0);
        source.resume.set_value(); CHECK(query.get() == 0);
        const auto result = pending.get();
        if (bad) {
            CHECK(result != 0 && !mapped);
            CHECK(!backend.space->ValidateRange({{a}, 0x4000}, GuestPermission::Read));
        } else {
            CHECK(result == 0 && mapped == reinterpret_cast<void*>(a));
            CHECK(std::memcmp(mapped, "DATA", 4) == 0);
            const auto* bytes = static_cast<const u8*>(mapped);
            CHECK(std::all_of(bytes + 4, bytes + 0x4000, [](u8 b) { return b == 0; }));
            CHECK(!backend.space->ValidateRange({{a}, 1}, GuestPermission::Write));
            CHECK(memory.UnmapMemory(a, 0x4000) == 0);
        }
    }
    // Real direct allocation + shared desktop pool path through checked guest ABI.
    using namespace Core::HostRuntime;
    using namespace Libraries::Kernel;
    auto call = [&](std::string_view nid, std::array<u64, 6> args) {
        return u32(DispatchDirectMemoryService(*backend.space, nid, args));
    };
    const u64 out = unrelated, pool = a + 0x200000;
    auto read64 = [&](u64 at) { u64 v{}; CHECK(bool(backend.space->ReadData({at}, std::as_writable_bytes(std::span{&v, 1})))); return v; };
    CHECK(call("B+vc2AO2Zrc", {0x4000, 0x4000, 0, 1}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(call("B+vc2AO2Zrc", {0x4000, 0x4000, 0, out}) == 0);
    const auto physical = read64(out);
    CHECK(call("hwVSPCmp5tM", {physical, 0x4000}) == 0);
    CHECK(call("hwVSPCmp5tM", {physical, 0x4000}) == u32(ORBIS_KERNEL_ERROR_ENOENT));
    CHECK(call("hwVSPCmp5tM", {~0x3fffULL, 0x8000}) == u32(ORBIS_KERNEL_ERROR_ENOENT));
    CHECK(call("C0f7TJcbfac", {1, 0x28000, 0x10000, out, out + 8}) == 0);
    CHECK(read64(out) == 0x10000 && read64(out + 8) == 0x18000);
    CHECK(call("C0f7TJcbfac", {UINT64_MAX, 0x28000, 0, out, out + 8}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(call("qCSfqDILlns", {0x10000, 0x20000, 0x20000, 0x10000, out}) != 0);
    // Exactly consume the expanded budget; reject further commits until decommit.
    CHECK(call("qCSfqDILlns", {0x10000, 0x30000, 0x20000, 0x10000, out}) == 0);
    CHECK(read64(out) == 0x10000);
    CHECK(call("pU-QydtGcGY", {pool, 0x200000, 0x200000, u64(MemoryMapFlags::Fixed), out}) == 0);
    CHECK(read64(out) == pool);
    CHECK(call("Vzl66WmfLvk", {pool, 0x20000, 3, u64(MemoryProt::CpuRead), 0}) == 0);
    CHECK(backend.space->ValidateRange({{pool}, 1}, GuestPermission::Read));
    CHECK(!backend.space->ValidateRange({{pool}, 1}, GuestPermission::Write));
    CHECK(call("Vzl66WmfLvk", {pool + 0x20000, 0x10000, 3, u64(MemoryProt::CpuReadWrite)}) == u32(ORBIS_KERNEL_ERROR_ENOMEM));
    CHECK(call("LXo1tpFqJGs", {pool + 1, 0x10000}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(call("LXo1tpFqJGs", {unrelated, 0x10000}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(call("LXo1tpFqJGs", {pool, 0x20000}) == 0);
    CHECK(!backend.space->ValidateRange({{pool}, 1}, GuestPermission::Read));
    // Sparse backing, partial decommit and recommit must preserve the other half.
    CHECK(call("Vzl66WmfLvk", {pool, 0x10000, 3, u64(MemoryProt::CpuReadWrite)}) == 0);
    CHECK(call("Vzl66WmfLvk", {pool + 0x10000, 0x10000, 3, u64(MemoryProt::CpuReadWrite)}) == 0);
    auto pin = backend.space->AcquireDataSpan({{pool}, 0x10000}, true);
    CHECK(bool(pin));
    auto retiring = std::async(std::launch::async, [&] { return call("LXo1tpFqJGs", {pool, 0x10000}); });
    CHECK(retiring.wait_for(50ms) == std::future_status::timeout);
    CHECK(call("bvD+95Q6asU", {out, 16}) == 0);
    if (pin) pin.Value() = {};
    CHECK(retiring.get() == 0);
    CHECK(backend.space->ValidateRange({{pool + 0x10000}, 0x10000}, GuestPermission::Write));
    CHECK(call("Vzl66WmfLvk", {pool, 0x10000, 3, u64(MemoryProt::CpuReadWrite)}) == 0);
    // Batch copies entries before retiring them; partial count survives a later refusal.
    std::array<OrbisKernelMemoryPoolBatchEntry, 2> entries{};
    entries[0].opcode = OrbisKernelMemoryPoolOpcode::Decommit;
    entries[0].decommit_params = {reinterpret_cast<void*>(pool), 0x10000};
    entries[1].opcode = OrbisKernelMemoryPoolOpcode::Move;
    CHECK(bool(backend.space->WriteData({pool}, std::as_bytes(std::span(entries)))));
    CHECK(call("YN878uKRBbE", {pool, 2, out}) == u32(ORBIS_KERNEL_ERROR_ENOSYS));
    u32 processed{};
    CHECK(bool(backend.space->ReadData({out}, std::as_writable_bytes(std::span{&processed, 1}))));
    CHECK(processed == 1);
    CHECK(call("bvD+95Q6asU", {1, 16}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(call("bvD+95Q6asU", {0, 0}) == 0);
    CHECK(call("LXo1tpFqJGs", {pool + 0x10000, 0x10000}) == 0);
    std::printf("guest_memory_manager_concurrency_tests: %u checks, %u failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
