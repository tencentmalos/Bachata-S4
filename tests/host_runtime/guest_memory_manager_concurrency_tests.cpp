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
#include "core/host_runtime/guest_memory_hle.h"
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
    bool disjoint{};
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
        if (disjoint) {
            result.add(boost::icl::interval<VAddr>::right_open(base, base + 0x200000));
            result.add(boost::icl::interval<VAddr>::right_open(base + 0x400000, base + 0x800000));
        } else {
            result.add(boost::icl::interval<VAddr>::right_open(base, base + 0x800000));
        }
        return result;
    }
    bool OwnsRange(VAddr a, u64 n) const override {
        if (disjoint && (a < base || a > UINT64_MAX - n ||
                         (a < base + 0x400000 && a + n > base + 0x200000)))
            return false;
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
// Distinct source bytes across noncontiguous backing must retain their offsets.
static void CheckBackingWrites() {
    {
        Backend b;
        MemoryManager m(&b);
        Memory::Binding bind(m);
        const auto a = b.base;
        CHECK(m.Allocate(0, 0x40000, 0x4000, 0x4000, 0) == 0);
        CHECK(m.Allocate(0x8000, 0x40000, 0x4000, 0x4000, 0) == 0x8000);
        void* out{};
        CHECK(m.MapMemory(&out, a, 0x4000, MemoryProt::CpuReadWrite, MemoryMapFlags::Fixed,
                          VMAType::Direct, "first", false, 0) == 0);
        CHECK(m.MapMemory(&out, a + 0x4000, 0x4000, MemoryProt::CpuReadWrite, MemoryMapFlags::Fixed,
                          VMAType::Direct, "second", false, 0x8000) == 0);
        std::array<u8, 0x8000> input{};
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = u8((i / 0x100) ^ i);
        for (const auto [start, count] :
             {std::pair<u64, u64>{0, 0x8000}, {0x321, 0x7123}, {0x3ff0, 0x40}}) {
            b.backing.fill(0xa5);
            CHECK(m.TryWriteBacking(reinterpret_cast<void*>(a + start), input.data(), count));
            auto expected = std::array<u8, 0x40000>{};
            expected.fill(0xa5);
            for (u64 i = 0; i < count; ++i) {
                const auto v = start + i;
                expected[v < 0x4000 ? v : 0x8000 + v - 0x4000] = input[i];
            }
            CHECK(b.backing == expected);
        }
        // A hole after valid backing must reject the whole batch without writing a prefix.
        b.backing.fill(0xa5);
        const auto before = b.backing;
        CHECK(!m.TryWriteBacking(reinterpret_cast<void*>(a + 0x7ff0), input.data(), 0x40));
        CHECK(b.backing == before);
    }
    {
        Backend b;
        MemoryManager m(&b);
        Memory::Binding bind(m);
        CHECK(m.PoolExpand(0, 0x40000, 0x10000, 0x10000) == 0);
        CHECK(m.PoolExpand(0x20000, 0x40000, 0x10000, 0x10000) == 0x20000);
        void* out{};
        CHECK(m.MapMemory(&out, b.base, 0x20000, MemoryProt::NoAccess, MemoryMapFlags::Fixed,
                          VMAType::PoolReserved, "pooled", false) == 0);
        CHECK(m.PoolCommit(b.base, 0x20000, MemoryProt::CpuReadWrite, 3) == 0);
        std::vector<u8> input(0x18000);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = u8((i / 0x100) ^ i);
        b.backing.fill(0xa5);
        CHECK(m.TryWriteBacking(reinterpret_cast<void*>(b.base + 0x1234), input.data(),
                                input.size()));
        auto expected = std::array<u8, 0x40000>{};
        expected.fill(0xa5);
        for (u64 i = 0; i < input.size(); ++i) {
            const auto v = 0x1234 + i;
            expected[v < 0x10000 ? v : 0x20000 + v - 0x10000] = input[i];
        }
        CHECK(b.backing == expected); // One VMA containing two physical spans.
    }
}
int main(int argc, char** argv) {
    CheckBackingWrites();
    if (argc == 2 && std::string_view(argv[1]) == "--backing-only") {
        std::printf("backing_write_tests: %u checks, %u failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    {
        const std::array<GuestRange, 2> proposed{
            {{{0x400000}, 0xfc00000}, {{0x100000000}, 0x100000000}}};
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
    // TypeProtect must drain I/O pins without blocking unrelated queries and
    // publish both real permissions and physical-type metadata before return.
    auto type_protect = [&](u64 address, u64 size, s32 type, s32 prot) {
        return Libraries::Kernel::sceKernelMtypeprotect(
            reinterpret_cast<void*>(address), size, type, prot);
    };
    exercise([&] { return type_protect(a + 1, 0x3fff, 3, 1); }, 1);
    Libraries::Kernel::OrbisVirtualQueryInfo typed{};
    Libraries::Kernel::OrbisQueryInfo physical_type{};
    CHECK(memory.VirtualQuery(a, 0, &typed) == 0);
    CHECK(typed.memory_type == 3 && typed.protection == 1);
    CHECK(memory.DirectMemoryQuery(0, false, &physical_type) == 0);
    CHECK(physical_type.memoryType == 3);
    CHECK(backend.space->ValidateRange({{a}, 0x4000}, GuestPermission::Read));
    CHECK(!backend.space->ValidateRange({{a}, 1}, GuestPermission::Write));
    CHECK(type_protect(a, 0x4000, -1, 3) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(type_protect(a, 0x4000, 11, 3) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(type_protect(UINT64_MAX - 1, 4, 0, 3) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(type_protect(0, UINT64_MAX, 0, 3) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(type_protect(0, 0x4000, 0, 3) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(memory.VirtualQuery(a, 0, &typed) == 0);
    CHECK(typed.memory_type == 3 && typed.protection == 1);
    CHECK(type_protect(a, 0, 0, 3) == 0);
    CHECK(type_protect(a, 0x4000, 0, 3) == 0);
    CHECK(memory.VirtualQuery(a, 0, &typed) == 0);
    CHECK(typed.memory_type == 0 && typed.protection == 3);
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
    const u64 stack = a + 0x180000;
    CHECK(map(stack, VMAType::Stack) == 0);
    CHECK(GuestQueryStack(*backend.space, memory, stack + 8, out, out + 8) == 0);
    CHECK(read64(out) == stack && read64(out + 8) == stack + 0x4000);
    CHECK(GuestQueryStack(*backend.space, memory, stack, 0, out) == 0);
    CHECK(read64(out) == stack + 0x4000);
    CHECK(GuestQueryStack(*backend.space, memory, stack, 0, 0) == 0);
    CHECK(GuestQueryStack(*backend.space, memory, stack, out, 1) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(read64(out) == stack + 0x4000); // no partial first output
    CHECK(GuestQueryStack(*backend.space, memory, unrelated, out, out + 8) == 0);
    CHECK(read64(out) == 0 && read64(out + 8) == 0);
    CHECK(GuestQueryStack(*backend.space, memory, UINT64_MAX, 0, 0) == u32(ORBIS_KERNEL_ERROR_EACCES));
    CHECK(memory.UnmapMemory(stack, 0x4000) == 0);
    CHECK(GuestQueryStack(*backend.space, memory, stack, out, out + 8) == u32(ORBIS_KERNEL_ERROR_EACCES));
    // MHW supplies a non-fixed 875.25 GiB hint. Search must wrap within the
    // owned envelope instead of failing while a suitable lower VMA is free.
    void* hinted{};
    CHECK(memory.MapMemory(&hinted, 0xdad0000000ULL, 0x4000, MemoryProt::NoAccess,
                           MemoryMapFlags::NoOverwrite, VMAType::Reserved, "high-hint",
                           false, -1, 0x10000) == 0);
    CHECK(backend.OwnsRange(reinterpret_cast<u64>(hinted), 0x4000));
    CHECK(reinterpret_cast<u64>(hinted) % 0x10000 == 0);
    CHECK(reinterpret_cast<u64>(hinted) != unrelated);
    CHECK(memory.UnmapMemory(reinterpret_cast<u64>(hinted), 0x4000) == 0);
    hinted = reinterpret_cast<void*>(0x1234);
    CHECK(memory.MapMemory(&hinted, 0xdad0000000ULL, 0x4000, MemoryProt::NoAccess,
                           MemoryMapFlags::Fixed, VMAType::Reserved, "fixed-outside") ==
          ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hinted == reinterpret_cast<void*>(0x1234));
    // An insufficient tail also wraps; an exhausted arena still fails.
    CHECK(memory.MapMemory(&hinted, a + 0x7fc000, 0x8000, MemoryProt::NoAccess,
                           MemoryMapFlags::NoFlags, VMAType::Reserved, "tail-hint") == 0);
    CHECK(backend.OwnsRange(reinterpret_cast<u64>(hinted), 0x8000));
    CHECK(memory.UnmapMemory(reinterpret_cast<u64>(hinted), 0x8000) == 0);
    CHECK(memory.MapMemory(&hinted, 0xdad0000000ULL, 0x1000000, MemoryProt::NoAccess,
                           MemoryMapFlags::NoFlags, VMAType::Reserved, "too-large") ==
          ORBIS_KERNEL_ERROR_ENOMEM);
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
    {
        Backend split;
        split.disjoint = true;
        MemoryManager segmented(&split);
        void* result{};
        const u64 gap = split.base + 0x300000;
        CHECK(segmented.MapMemory(&result, gap, 0x4000, MemoryProt::NoAccess,
                                  MemoryMapFlags::NoFlags, VMAType::Reserved, "gap-hint") == 0);
        CHECK(reinterpret_cast<u64>(result) == split.base + 0x400000);
        CHECK(segmented.UnmapMemory(reinterpret_cast<u64>(result), 0x4000) == 0);
        CHECK(segmented.MapMemory(&result, gap, 0x4000, MemoryProt::NoAccess,
                                  MemoryMapFlags::Fixed, VMAType::Reserved, "fixed-gap") ==
              ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(segmented.MapMemory(&result, 0xdad0000000ULL, 0x300000, MemoryProt::NoAccess,
                                  MemoryMapFlags::NoFlags, VMAType::Reserved, "large-high-hint") == 0);
        CHECK(reinterpret_cast<u64>(result) == split.base + 0x400000);
        CHECK(segmented.UnmapMemory(reinterpret_cast<u64>(result), 0x300000) == 0);
    }
    std::printf("guest_memory_manager_concurrency_tests: %u checks, %u failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
