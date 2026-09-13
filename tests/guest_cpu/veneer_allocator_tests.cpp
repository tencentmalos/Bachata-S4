// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone contract test for the HLE veneer allocator. Links guest_cpu_api
// only (no FEX, no JIT), so it runs on the CI host and cross-compiles under the
// NDK. It checks the emitted stub bytes, operation caching, slab exhaustion and
// the seal transition; the actual guest-call-through-a-veneer round trip is in
// tests/guest_cpu/guest_execution_tests.cpp, which needs the FEX backend.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/hle/veneer_allocator.h"

using namespace Core::GuestCpu;
using Core::GuestCpu::Hle::HleVeneerAllocator;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(const char* name, bool condition, const std::string& detail = {}) {
    ++g_checks;
    std::printf("[%-4d] %-64s %s", g_checks, name, condition ? "PASS" : "FAIL");
    if (!detail.empty()) {
        std::printf(" -- %s", detail.c_str());
    }
    std::printf("\n");
    if (!condition) {
        ++g_failures;
    }
}

std::string Hex(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// Reads the 16-byte veneer at `va` and checks it is exactly the expected stub for
// `operation`.
bool VeneerBytesMatch(GuestAddressSpace& space, GuestAddress va, std::uint64_t operation,
                      std::string& detail) {
    std::array<std::byte, HleVeneerAllocator::kVeneerSize> got{};
    if (auto r = space.Read(va, got); !r) {
        detail = "read failed";
        return false;
    }
    std::array<std::uint8_t, HleVeneerAllocator::kVeneerSize> want{};
    want[0] = 0x49;
    want[1] = 0x89;
    want[2] = 0xca;  // mov r10, rcx
    want[3] = 0x48;
    want[4] = 0xb8;  // movabs rax, imm64
    std::memcpy(want.data() + 5, &operation, sizeof(operation));
    want[13] = 0x0f;
    want[14] = 0x05;  // syscall
    want[15] = 0xc3;  // ret
    if (std::memcmp(got.data(), want.data(), want.size()) != 0) {
        detail = "stub bytes differ from the expected mov/movabs/syscall/ret";
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("HLE veneer allocator contract\n");
    const std::uint64_t page = HostPageSize();

    AddressSpaceConfig config{};
    config.reservation_size = page * 64;
    auto space_r = GuestAddressSpace::Create(config);
    if (!space_r) {
        std::printf("FAILED: create address space\n");
        return 1;
    }
    auto space = std::move(space_r).Value();
    const std::uint64_t base = space->ReservationBase().value;

    // A slab of two host pages inside the reservation, offset so it does not
    // collide with anything a real loader would map at the base.
    const GuestAddress slab_base{base + page * 8};
    const GuestRange slab{slab_base, page * 2};

    auto alloc_r = HleVeneerAllocator::Create(*space, slab);
    Check("Create maps the slab", bool(alloc_r),
          alloc_r ? std::string{} : Describe(alloc_r.GetError()));
    if (!alloc_r) {
        return 1;
    }
    auto alloc = std::move(alloc_r).Value();

    Check("operation zero is refused without consuming a slot",
          !alloc.Allocate(0) && alloc.Count() == 0);

    // Allocate a handful of operations; verify each stub and that they are packed
    // 16 bytes apart in allocation order.
    const std::uint64_t ops[] = {1, 2, 0x123456789abcdef0ULL, 7};
    GuestAddress vas[4]{};
    bool all_bytes_ok = true;
    for (int i = 0; i < 4; ++i) {
        auto v = alloc.Allocate(ops[i]);
        if (!v) {
            Check("Allocate returns a VA", false, Describe(v.GetError()));
            return 1;
        }
        vas[i] = v.Value();
        std::string detail;
        if (!VeneerBytesMatch(*space, vas[i], ops[i], detail)) {
            all_bytes_ok = false;
            Check("veneer bytes match the op", false, detail + " op=" + Hex(ops[i]));
        }
    }
    Check("every veneer emitted the exact stub with its op", all_bytes_ok);
    Check("veneers are packed 16 bytes apart",
          vas[1].value == vas[0].value + 16 && vas[2].value == vas[1].value + 16 &&
              vas[3].value == vas[2].value + 16,
          Hex(vas[0].value) + " " + Hex(vas[1].value) + " " + Hex(vas[2].value) + " " +
              Hex(vas[3].value));
    Check("first veneer sits at the slab base", vas[0].value == slab_base.value);

    // Re-allocating the same op returns the cached VA and does not advance.
    auto again = alloc.Allocate(ops[2]);
    Check("re-allocating an op returns the cached VA",
          again && again.Value().value == vas[2].value);
    Check("op count reflects unique operations only", alloc.Count() == 4,
          "count=" + std::to_string(alloc.Count()));
    Check("populated range spans exactly the used slots",
          alloc.PopulatedRange().size == 4 * 16,
          "size=" + Hex(alloc.PopulatedRange().size));

    // Execution admission prevents publishing RX; refusal leaves the allocator retryable.
    {
        auto lease = space->AcquireExecutionLease();
        auto refused = alloc.Seal();
        auto mapping = space->Query(slab.base);
        Check("Seal during execution refuses and keeps NX",
              lease && !refused && !alloc.IsSealed() && mapping &&
                  !HasPermission(mapping.Value().permission, GuestPermission::Execute));
    }
    // Seal makes the slab executable and refuses further allocation.
    auto sealed = alloc.Seal();
    Check("Seal succeeds", bool(sealed), sealed ? std::string{} : Describe(sealed.GetError()));
    Check("allocator reports sealed", alloc.IsSealed());
    auto after_seal = alloc.Allocate(99);
    Check("Allocate after Seal is refused", !after_seal,
          after_seal ? "unexpectedly succeeded" : std::string{});
    // The veneer bytes survive the RW->RX transition (a read still sees the stub).
    std::string post_seal_detail;
    Check("veneer bytes are intact after sealing",
          VeneerBytesMatch(*space, vas[0], ops[0], post_seal_detail), post_seal_detail);

    std::string padding_detail;
    Check(
        "unused aligned tail slot traps through reserved op zero",
        VeneerBytesMatch(*space, GuestAddress{slab.base.value + slab.size - 16}, 0, padding_detail),
        padding_detail);
    auto tail = space->Query(GuestAddress{slab.base.value + slab.size - 16});
    Check("published mapping is entirely RX",
          tail && tail.Value().permission == (GuestPermission::Read | GuestPermission::Execute));
    auto empty = HleVeneerAllocator::Create(*space, {GuestAddress{base + page * 24}, page});
    Check("empty sealed slab stays NX",
          empty && empty.Value().Seal() &&
              !HasPermission(space->Query(GuestAddress{base + page * 24}).Value().permission,
                             GuestPermission::Execute));

    // A slab smaller than one veneer is refused at creation.
    auto tiny = HleVeneerAllocator::Create(*space, GuestRange{GuestAddress{base + page * 16}, 8});
    Check("a slab smaller than one veneer is refused", !tiny);

    // Exhaustion: a one-page slab holds page/16 veneers; the next fails cleanly.
    auto small_r =
        HleVeneerAllocator::Create(*space, GuestRange{GuestAddress{base + page * 20}, page});
    if (small_r) {
        auto small = std::move(small_r).Value();
        const std::uint64_t capacity = page / 16;
        bool filled = true;
        for (std::uint64_t i = 0; i < capacity; ++i) {
            if (!small.Allocate(1000 + i)) {
                filled = false;
                break;
            }
        }
        Check("a full slab accepts exactly capacity veneers", filled,
              "capacity=" + std::to_string(capacity));
        auto overflow = small.Allocate(999999);
        Check("allocating past the slab is refused with OutOfMemory",
              !overflow && overflow.GetError().category == ErrorCategory::OutOfMemory);
    } else {
        Check("create small slab", false, Describe(small_r.GetError()));
    }

    std::printf("\n%d check(s), %s (%d failure%s)\n", g_checks,
                g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
