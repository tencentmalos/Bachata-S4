// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <future>
#include <thread>
#include "core/host_runtime/guest_clock.h"
#include "core/host_runtime/guest_mutex.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
int main() {
    unsigned checks{}, failures{};
    auto check = [&](const char* name, bool ok) {
        ++checks;
        failures += !ok;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    };
    AddressSpaceConfig cfg{};
    cfg.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(cfg);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    const auto rw = GuestPermission::Read | GuestPermission::Write;
    if (!space->Map({GuestAddress{base}, 0x4000}, rw))
        return 2;
    u64 next = base + 0x4000;
    GuestMutexDomain domain(*space, [&] {
        auto addr = next;
        next += 0x4000;
        if (!space->Map({GuestAddress{addr}, 0x4000}, rw))
            throw std::runtime_error("allocate");
        return addr;
    });
    auto write = [&](u64 addr, u64 value) {
        return bool(space->Write(GuestAddress{addr}, std::as_bytes(std::span{&value, 1})));
    };
    auto read = [&](u64 addr) {
        u64 value{};
        if (!space->Read(GuestAddress{addr}, std::as_writable_bytes(std::span{&value, 1})))
            throw std::runtime_error("read");
        return value;
    };
    const u64 slot = base, attr = base + 8, cond = base + 16;
    check("default init", domain.Init(slot, 0) == 0);
    check("guest ABI prefix flags", (read(read(slot) + 0x20) & 0xffffffff) == 1);
    check("lock owner", domain.Lock(slot, 101, false, {}) == 0);
    check("recursive errorcheck", domain.Lock(slot, 101, false, {}) == POSIX_EDEADLK);
    check("foreign unlock", domain.Unlock(slot, 202) == POSIX_EPERM);
    check("busy trylock", domain.Lock(slot, 202, true, {}) == POSIX_EBUSY);
    check("owned destroy busy", domain.Destroy(slot) == POSIX_EBUSY);
    std::stop_source cancel;
    auto waiting = std::async(std::launch::async,
                              [&] { return domain.Lock(slot, 202, false, cancel.get_token()); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!domain.PendingWaits(slot) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check("waiter actually queued", domain.PendingWaits(slot) == 1);
    cancel.request_stop();
    check("cancel lock bounded",
          waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    check("cancel lock result", waiting.get() == POSIX_EINTR);
    check("owner retained after cancellation", domain.IsOwned(slot, 101));
    check("unlock", domain.Unlock(slot, 101) == 0);
    check("destroy", domain.Destroy(slot) == 0);
    check("stale handle", domain.Lock(slot, 101, false, {}) == POSIX_EINVAL);
    check("attribute init", domain.AttributeInit(attr) == 0);
    check("recursive type", domain.Attribute(attr, 2, 1) == 0);
    write(slot, 0);
    check("recursive init", domain.Init(slot, attr) == 0);
    check("recursive lock twice",
          domain.Lock(slot, 101, false, {}) == 0 && domain.Lock(slot, 101, false, {}) == 0);
    check("recursive unlock twice", domain.Unlock(slot, 101) == 0 && domain.Unlock(slot, 101) == 0);
    check("bad attribute type", domain.Attribute(attr, 0, 1) == POSIX_EINVAL);
    check("attribute destroy", domain.Attribute(attr, 0, 0) == 0);
    check("stale attribute", domain.Attribute(attr, 2, 1) == POSIX_EINVAL);
    check("condition init", domain.CondInit(cond, 0) == 0);
    check("condition no waiter broadcast", domain.CondNotify(cond, true) == 0);
    check("condition owner lock", domain.Lock(slot, 101, false, {}) == 0);
    auto condition =
        std::async(std::launch::async, [&] { return domain.CondWait(cond, slot, 101, {}); });
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!domain.PendingWaits(slot) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check("condition releases mutex", domain.Lock(slot, 202, true, {}) == 0);
    check("condition destroy busy", domain.CondDestroy(cond) == POSIX_EBUSY);
    check("signal", domain.CondNotify(cond, false) == 0);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!domain.PendingReacquires(cond) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check("notified waiter is reacquiring", domain.PendingReacquires(cond) == 1);
    check("condition remains alive during reacquire", domain.CondDestroy(cond) == POSIX_EBUSY);
    check("wait must reacquire",
          condition.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    check("signaller unlock", domain.Unlock(slot, 202) == 0);
    check("condition returned with mutex", condition.get() == 0 && domain.IsOwned(slot, 101));
    std::stop_source stop;
    auto cancelled = std::async(std::launch::async,
                                [&] { return domain.CondWait(cond, slot, 101, stop.get_token()); });
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!domain.PendingWaits(slot) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check("second condition releases", domain.Lock(slot, 202, true, {}) == 0);
    stop.request_stop();
    check("terminal cancel does not wait for stopped owner",
          cancelled.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    check("condition cancelled", cancelled.get() == POSIX_EINTR);
    check("no dangling waiter", domain.PendingWaits(slot) == 0 && domain.CondDestroy(cond) == 0);
    check("other owner retained", domain.IsOwned(slot, 202) && domain.Unlock(slot, 202) == 0);
    GuestClock clock;
    Libraries::Kernel::OrbisKernelTimespec ts{};
    check("clock initialized outside desktop registry",
          clock.Read(15, ts, false) == 0 && clock.ticks.GetTscFrequency() > 0);
    check("bad clock is errno", clock.Read(UINT32_MAX, ts, false) == POSIX_EINVAL);
    std::chrono::nanoseconds ns;
    check("duration overflow rejected", !GuestClock::Duration({INT64_MAX, 0}, ns));
    check("invalid nanoseconds rejected", !GuestClock::Duration({0, 1000000000}, ns));
    check("duration zero", GuestClock::Duration({0, 0}, ns) && ns.count() == 0);
    std::printf("GUEST_SERVICES checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
