// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <future>
#include <thread>
#include "core/host_runtime/guest_mutex.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
static std::array<GuestMutexDomain::CondTraceEvent, 128> trace_events{};
static size_t trace_count{};
static std::mutex trace_guard;
static void Observe(const GuestMutexDomain::CondTraceEvent& event) noexcept {
    std::lock_guard lock(trace_guard);
    if (trace_count < trace_events.size()) trace_events[trace_count] = event;
    ++trace_count;
}
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__);                             \
        }                                                                                          \
    } while (0)
int main(int argc, char**) {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(
        space->Map({GuestAddress{base}, 0x20000}, GuestPermission::Read | GuestPermission::Write));
    u64 next = base + 4096;
    GuestMutexDomain domain(*space, [&] {
        auto value = next;
        next += 64;
        return value;
    });
    if (argc > 1) domain.SetCondObserver(Observe);
    auto read = [&]<class T>(u64 address) {
        T value{};
        CHECK(space->Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    auto wait = [&](auto predicate) {
        const auto end = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        return predicate();
    };
    using Op = GuestMutexDomain::CondAttrOp;
    const u64 attr = base, cond = base + 8, mutex = base + 16, out = base + 24;
    CHECK(domain.CondAttribute(1, 0, Op::Init) == POSIX_EFAULT);
    CHECK(domain.CondAttribute(attr, 0, Op::Init) == 0);
    CHECK(domain.CondAttribute(attr, 0, Op::Init) == POSIX_EBUSY);
    CHECK(domain.CondAttribute(attr, out, Op::GetClock) == 0 && read.operator()<u32>(out) == 0);
    for (u64 clock : {0, 1, 2, 4}) {
        CHECK(domain.CondAttribute(attr, clock, Op::SetClock) == 0);
        CHECK(domain.CondAttribute(attr, out, Op::GetClock) == 0 &&
              read.operator()<u32>(out) == clock);
    }
    CHECK(domain.CondAttribute(attr, 3, Op::SetClock) == POSIX_EINVAL);
    CHECK(domain.CondAttribute(attr, 1, Op::SetShared) == POSIX_EINVAL);
    CHECK(domain.CondAttribute(attr, 0, Op::SetShared) == 0);
    CHECK(domain.CondAttribute(attr, out, Op::GetShared) == 0 && read.operator()<u32>(out) == 0);
    CHECK(domain.CondAttribute(attr, 1, Op::GetClock) == POSIX_EFAULT);
    CHECK(domain.CondInit(1, attr) == POSIX_EFAULT);
    CHECK(domain.CondInit(cond, 1) == POSIX_EFAULT);
    CHECK(domain.CondInit(cond, attr) == 0); // captures monotonic
    CHECK(domain.CondAttribute(attr, 0, Op::SetClock) == 0);
    CHECK(domain.Init(mutex, 0) == 0);
    CHECK(domain.Lock(mutex, 101, false, {}) == 0);
    CHECK(domain.CondWait(1, mutex, 101, {}, {.relative_us = 0}) == POSIX_EFAULT);
    CHECK(domain.IsOwned(mutex, 101));
    CHECK(domain.CondWait(cond, mutex, 202, {}, {.relative_us = 0}) == POSIX_EPERM);
    CHECK(domain.CondWait(cond, mutex, 101, {}, {.relative_us = 0}) == POSIX_ETIMEDOUT);
    CHECK(domain.IsOwned(mutex, 101));
    GuestClock clock;
    Libraries::Kernel::OrbisKernelTimespec ts{};
    std::chrono::nanoseconds absolute{};
    CHECK(clock.Read(4, ts, false) == 0 && GuestClock::Duration(ts, absolute));
    const auto begin = std::chrono::steady_clock::now();
    CHECK(domain.CondWait(cond, mutex, 101, {}, {.absolute = absolute + 20ms}) == POSIX_ETIMEDOUT);
    CHECK(std::chrono::steady_clock::now() - begin >= 10ms && domain.IsOwned(mutex, 101));
    CHECK(domain.CondWait(cond, mutex, 101, {}, {.absolute = 0ns}) == POSIX_ETIMEDOUT);
    // Expiration does not return before mutex reacquisition; condition lifetime
    // includes that phase, even though it no longer accepts notifications.
    std::stop_source cancel;
    auto timed = std::async(std::launch::async, [&] {
        return domain.CondWait(cond, mutex, 101, cancel.get_token(), {.relative_us = 10000});
    });
    CHECK(wait([&] { return domain.PendingWaits(mutex) == 1; }));
    CHECK(domain.Lock(mutex, 202, false, {}) == 0);
    CHECK(wait([&] { return domain.PendingReacquires(cond) == 1; }));
    CHECK(timed.wait_for(0ms) == std::future_status::timeout);
    CHECK(domain.CondDestroy(cond) == POSIX_EBUSY);
    CHECK(domain.CondNotify(cond, false, 101) == POSIX_EPERM);
    CHECK(domain.Unlock(mutex, 202) == 0);
    CHECK(timed.get() == POSIX_ETIMEDOUT && domain.IsOwned(mutex, 101));
    // Two actual waiters on one mutex, a targeted wake must pick owner 202.
    auto first = std::async(std::launch::async,
                            [&] { return domain.CondWait(cond, mutex, 101, cancel.get_token()); });
    CHECK(wait([&] { return domain.PendingWaits(mutex) == 1; }));
    CHECK(domain.Lock(mutex, 202, false, {}) == 0);
    auto second = std::async(std::launch::async,
                             [&] { return domain.CondWait(cond, mutex, 202, cancel.get_token()); });
    CHECK(wait([&] { return domain.PendingWaits(mutex) == 2; }));
    CHECK(domain.CondNotify(cond, false, 202) == 0);
    CHECK(wait([&] { return second.wait_for(0ms) == std::future_status::ready; }));
    CHECK(second.get() == 0 && domain.IsOwned(mutex, 202));
    CHECK(first.wait_for(0ms) == std::future_status::timeout);
    CHECK(domain.Unlock(mutex, 202) == 0);
    CHECK(domain.CondNotify(cond, false, 101) == 0);
    CHECK(first.get() == 0 && domain.IsOwned(mutex, 101));
    auto cancelled = std::async(std::launch::async, [&] {
        return domain.CondWait(cond, mutex, 101, cancel.get_token(), {.relative_us = UINT64_MAX});
    });
    CHECK(wait([&] { return domain.PendingWaits(mutex) == 1; }));
    CHECK(domain.Lock(mutex, 202, false, {}) == 0);
    cancel.request_stop();
    CHECK(cancelled.get() == POSIX_EINTR && domain.IsOwned(mutex, 202));
    CHECK(domain.PendingWaits(mutex) == 0 && domain.PendingReacquires(cond) == 0);
    CHECK(domain.CondDestroy(cond) == 0);
    CHECK(domain.Unlock(mutex, 202) == 0 && domain.Destroy(mutex) == 0);
    CHECK(domain.CondAttribute(attr, 0, Op::Destroy) == 0);
    CHECK(domain.CondAttribute(attr, out, Op::GetClock) == POSIX_EINVAL);
    CHECK(domain.CondAttribute(attr, 0, Op::Destroy) == POSIX_EINVAL);
    if (argc > 1) {
        CHECK(trace_count > 0 && trace_count <= trace_events.size());
        std::array<size_t, 4> phases{};
        for (size_t i = 0; i < std::min(trace_count, trace_events.size()); ++i) {
            const auto& e = trace_events[i];
            CHECK(e.condition != 0 && e.mutex != 0 && e.owner != 0);
            ++phases[static_cast<size_t>(e.kind)];
        }
        CHECK(phases[0] == phases[2] && phases[2] == phases[3]);
        CHECK(phases[1] > 0 && phases[1] <= phases[0]);
    } else CHECK(trace_count == 0);
    std::printf("GUEST_CONDITION checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
