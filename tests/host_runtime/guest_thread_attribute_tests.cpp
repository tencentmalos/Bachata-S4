// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_thread_attributes.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
int main() {
    unsigned checks{}, failures{};
    auto check = [&](bool pass) {
        ++checks;
        if (!pass) {
            ++failures;
            std::printf("FAIL %u\n", checks);
        }
    };
    AddressSpaceConfig config{};
    config.reservation_size = 65536;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    if (!space->Map({GuestAddress{base}, 65536}, GuestPermission::Read | GuestPermission::Write))
        return 3;
    u64 allocation = base + 4096;
    GuestThreadAttributeDomain domain(*space, [&] {
        allocation += 64;
        return allocation;
    });
    auto invoke = [&](ThreadAttrOp op, bool get, u64 slot, u64 a = 0, u64 b = 0) {
        return domain.Invoke({"", op, get}, slot, a, b);
    };
    auto put = [&](u64 address, auto value) {
        check(bool(space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1}))));
    };
    GuestThreadAttributes snapshot;
    check(domain.Snapshot(0, snapshot) == 0 && snapshot.size == (1 << 20));
    check(invoke(ThreadAttrOp::Init, false, 0) == POSIX_EFAULT);
    check(invoke(ThreadAttrOp::Init, false, base) == 0);
    u64 handle{};
    check(bool(space->Read(GuestAddress{base}, std::as_writable_bytes(std::span{&handle, 1}))));
    check(handle == allocation);
    check(invoke(ThreadAttrOp::Init, false, base) == POSIX_EBUSY);
    check(invoke(ThreadAttrOp::StackSize, false, base, 0x3fff) == POSIX_EINVAL);
    check(invoke(ThreadAttrOp::Stack, false, base, UINT64_MAX - 31, 0x4000) == POSIX_EINVAL);
    check(invoke(ThreadAttrOp::StackSize, false, base, GuestThreadAttributeDomain::MaxStack + 1) ==
          POSIX_EINVAL);
    check(invoke(ThreadAttrOp::StackSize, false, base, 0x40000) == 0);
    check(domain.Snapshot(base, snapshot) == 0 && snapshot.size == 0x40000);
    check(invoke(ThreadAttrOp::Guard, false, base, 0x8000) == 0);
    check(invoke(ThreadAttrOp::Detach, false, base, 2) == POSIX_EINVAL);
    check(invoke(ThreadAttrOp::Detach, false, base, 1) == 0);
    check(invoke(ThreadAttrOp::Inherit, false, base, 3) == POSIX_ENOTSUP);
    check(invoke(ThreadAttrOp::Inherit, false, base, 0) == 0);
    check(invoke(ThreadAttrOp::Policy, false, base, 2) == 0);
    put(base + 128, s32{800});
    check(invoke(ThreadAttrOp::Priority, false, base, base + 128) == 0);
    check(invoke(ThreadAttrOp::Priority, false, base, 0) == POSIX_EFAULT);
    check(invoke(ThreadAttrOp::Scope, false, base, 0) ==
          0); // Desktop's OR predicate rejects both valid values.
    check(invoke(ThreadAttrOp::Scope, false, base, 1) == POSIX_EINVAL);
    check(invoke(ThreadAttrOp::Scope, false, base, 2) == 0);
    check(invoke(ThreadAttrOp::Suspend, false, base) == POSIX_ENOTSUP);
    put(base + 144, u64{1});
    put(base + 152, u64{0});
    check(invoke(ThreadAttrOp::Affinity, false, base, 16, base + 144) == 0);
    put(base + 144, u64{0});
    check(domain.Snapshot(base, snapshot) == 0 && snapshot.affinity == 1 &&
          snapshot.guard == 0x8000 && snapshot.detached == 1 && snapshot.priority == 800 &&
          snapshot.inherit == 0);
    check(invoke(ThreadAttrOp::Affinity, false, base, 16, base + 144) == POSIX_EINVAL);
    // The same checked helpers serve affinity changes on live runtime owners.
    check(invoke(ThreadAttrOp::AffinityMask, false, base, 0x40) == 0);
    check(invoke(ThreadAttrOp::AffinityMask, true, base, base + 192) == 0);
    u64 affinity{};
    check(bool(space->ReadData(GuestAddress{base + 192}, std::as_writable_bytes(std::span{&affinity, 1}))) && affinity == 0x40);
    check(invoke(ThreadAttrOp::AffinityMask, false, base, 0x100) == POSIX_EINVAL);
    check(domain.Snapshot(base, snapshot) == 0 && snapshot.affinity == 0x40);
    check(invoke(ThreadAttrOp::Affinity, true, base, 129, base + 192) == POSIX_EINVAL);
    check(invoke(ThreadAttrOp::Affinity, true, base, 8, 0) == POSIX_EFAULT);
    check(invoke(ThreadAttrOp::Affinity, false, base, 8, 1) == POSIX_EFAULT);
    check(domain.Snapshot(base, snapshot) == 0 && snapshot.affinity == 0x40);
    check(invoke(ThreadAttrOp::Affinity, false, base, 0, 0) == 0);
    check(invoke(ThreadAttrOp::AffinityMask, true, base, base + 192) == 0);
    check(bool(space->ReadData(GuestAddress{base + 192}, std::as_writable_bytes(std::span{&affinity, 1}))) && affinity == 0xff);
    check(invoke(ThreadAttrOp::Stack, true, base, base + 160, 1) == POSIX_EFAULT);
    put(base + 160, u64{0xaabb});
    check(invoke(ThreadAttrOp::Stack, true, base, base + 160, 1) == POSIX_EFAULT);
    u64 untouched{};
    check(bool(space->Read(GuestAddress{base + 160},
                           std::as_writable_bytes(std::span{&untouched, 1}))) &&
          untouched == 0xaabb);
    check(invoke(ThreadAttrOp::Stack, false, base, base + 0x8000, 0x4000) == 0);
    check(domain.Snapshot(base, snapshot) == 0 && snapshot.stack == base + 0x8000 &&
          snapshot.size == 0x4000);
    put(base + 8, handle); // Saved alias becomes stale after destroy.
    check(invoke(ThreadAttrOp::Destroy, false, base) == 0);
    check(domain.Snapshot(base + 8, snapshot) == POSIX_EINVAL);
    check(invoke(ThreadAttrOp::Init, false, base) == 0);
    check(domain.Snapshot(base + 8, snapshot) == POSIX_EINVAL);
    check(domain.Snapshot(base, snapshot) == 0 && snapshot.stack == 0 && snapshot.detached == 0);
    check(invoke(ThreadAttrOp::Destroy, false, base) == 0);
    std::printf("THREAD_ATTRIBUTES checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
