// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <thread>
#if defined(__linux__)
#include <sys/resource.h>
#endif
#include "core/host_runtime/guest_kernel_semaphore.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig cfg{};
    cfg.reservation_size = 1 << 20;
    auto made = GuestAddressSpace::Create(cfg);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    GuestKernelSemaphore sem(*space);
    auto put = [&](u64 a, const auto& v) {
        CHECK(space->Write(GuestAddress{a}, std::as_bytes(std::span{&v, 1})));
    };
    auto get = [&]<class T>(u64 a) {
        T v{};
        CHECK(space->Read(GuestAddress{a}, std::as_writable_bytes(std::span{&v, 1})));
        return v;
    };
    auto call = [&](std::string_view n, std::array<u64, 6> a = {}) {
        return sem.Dispatch(n, a, 700);
    };
    auto entered = [&](u32 id, u32 n) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (sem.Waiting(id) != n && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        CHECK(sem.Waiting(id) == n);
    };
    put(base + 0x100, std::array<char, 4>{'a', 'b', 'c', 0});
    CHECK(call("188x57JYp0g", {base, base + 0x100, 3, 0, 3, 0}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(call("188x57JYp0g", {1, base + 0x100, 1, 0, 3, 0}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(call("188x57JYp0g", {base, base + 0x100, 1, 0, 3, 0}) == 0);
    u32 id = get.operator()<u32>(base);
    CHECK(call("12wOHk8ywb0", {id, 1}) == u32(ORBIS_KERNEL_ERROR_EBUSY));
    CHECK(call("4czppHBiriw", {id, 4}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(call("Zxa0VhQVTsk", {id, 1, 1}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    put(base + 8, u32(1000));
    CHECK(call("Zxa0VhQVTsk", {id, 1, base + 8}) == u32(ORBIS_KERNEL_ERROR_ETIMEDOUT));
    CHECK(get.operator()<u32>(base + 8) == 0);
    CHECK(call("4czppHBiriw", {id, 3}) == 0);
    CHECK(call("12wOHk8ywb0", {id, 2}) == 0);
    CHECK(call("12wOHk8ywb0", {id, 2}) == u32(ORBIS_KERNEL_ERROR_EBUSY));
    CHECK(call("12wOHk8ywb0", {id, 1}) == 0);
    u64 first{}, second{};
    std::jthread w1(
        [&](std::stop_token s) { first = sem.Dispatch("Zxa0VhQVTsk", {id, 2, 0}, 700, s); });
    entered(id, 1);
    std::jthread w2(
        [&](std::stop_token s) { second = sem.Dispatch("Zxa0VhQVTsk", {id, 1, 0}, 500, s); });
    entered(id, 2);
    CHECK(call("4czppHBiriw", {id, 1}) == 0);
    w2.join();
    CHECK(second == 0);
    entered(id, 1); // desktop skips unsatisfiable first waiter
    CHECK(call("4DM06U2BNEY", {id, 0, base + 8}) == 0);
    w1.join();
    CHECK(first == u32(ORBIS_KERNEL_ERROR_ECANCELED));
    CHECK(get.operator()<s32>(base + 8) == 1);
    std::jthread w3(
        [&](std::stop_token s) { first = sem.Dispatch("Zxa0VhQVTsk", {id, 1, 0}, 700, s); });
    entered(id, 1);
    w3.request_stop();
    w3.join();
    CHECK(first == u32(ORBIS_KERNEL_ERROR_EINTR));
    entered(id, 0);
    std::jthread w4(
        [&](std::stop_token s) { first = sem.Dispatch("Zxa0VhQVTsk", {id, 1, 0}, 700, s); });
    entered(id, 1);
    CHECK(call("R1Jvn8bSCW8", {id}) == 0);
    w4.join();
    CHECK(first == u32(ORBIS_KERNEL_ERROR_EACCES));
    CHECK(call("12wOHk8ywb0", {id, 1}) == u32(ORBIS_KERNEL_ERROR_ESRCH));
    // Priority order when both waiters can consume the same signal.
    CHECK(call("188x57JYp0g", {base, base + 0x100, 2, 0, 3, 0}) == 0);
    u32 priority_id = get.operator()<u32>(base);
    CHECK(priority_id != id);
    std::jthread p1([&](std::stop_token s) {
        first = sem.Dispatch("Zxa0VhQVTsk", {priority_id, 1, 0}, 700, s);
    });
    entered(priority_id, 1);
    std::jthread p2([&](std::stop_token s) {
        second = sem.Dispatch("Zxa0VhQVTsk", {priority_id, 1, 0}, 500, s);
    });
    entered(priority_id, 2);
    CHECK(call("4czppHBiriw", {priority_id, 1}) == 0);
    p2.join();
    CHECK(second == 0);
    entered(priority_id, 1);
    CHECK(call("4DM06U2BNEY", {priority_id, 0, 1}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    entered(priority_id, 1);
    CHECK(call("4DM06U2BNEY", {priority_id, 0, base + 8}) == 0);
    p1.join();
    CHECK(first == u32(ORBIS_KERNEL_ERROR_ECANCELED));
    put(base + 8, u32(1000000));
    std::jthread timed([&](std::stop_token s) {
        first = sem.Dispatch("Zxa0VhQVTsk", {priority_id, 1, base + 8}, 700, s);
    });
    entered(priority_id, 1);
    {
        auto token = space->Quiesce(0);
        CHECK(token);
        if (token)
            CHECK(space->UpdateVmUnderToken(token.Value(), GuestAddressSpace::VmOperation::Map,
                                            {GuestAddress{base}, HostPageSize()},
                                            GuestPermission::Read | GuestPermission::Write));
    }
    put(base + 8, u32(0xdeadbeef));
    CHECK(call("4czppHBiriw", {priority_id, 1}) == 0);
    timed.join();
    CHECK(first == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(get.operator()<u32>(base + 8) == 0xdeadbeef);
    CHECK(call("R1Jvn8bSCW8", {priority_id}) == 0);
    // A signal with no satisfiable waiter must not wake unrelated objects or
    // the unsatisfied waiter on the same object. Count actual kernel context
    // switches (not wall time or production instrumentation). The old domain
    // broadcast wakes both threads on nearly every iteration.
#if defined(__linux__)
    CHECK(call("188x57JYp0g", {base, base + 0x100, 1, 0, 3, 0}) == 0);
    const u32 active_id = get.operator()<u32>(base);
    CHECK(call("188x57JYp0g", {base + 4, base + 0x100, 1, 0, 3, 0}) == 0);
    const u32 idle_id = get.operator()<u32>(base + 4);
    std::array<long, 2> switches{};
    std::array<u64, 2> results{};
    auto unrelated_wait = [&](unsigned index, u32 target, s32 need) {
        rusage before{}, after{};
        const int a = getrusage(RUSAGE_THREAD, &before);
        results[index] = sem.Dispatch("Zxa0VhQVTsk", {target, u64(need), 0}, 700);
        const int b = getrusage(RUSAGE_THREAD, &after);
        switches[index] = a || b ? -1 : after.ru_nvcsw - before.ru_nvcsw;
    };
    std::jthread idle([&] { unrelated_wait(0, idle_id, 1); });
    std::jthread unsatisfied([&] { unrelated_wait(1, active_id, 2); });
    entered(idle_id, 1);
    entered(active_id, 1);
    for (unsigned n = 0; n < 32; ++n) {
        CHECK(call("4czppHBiriw", {active_id, 1}) == 0);
        CHECK(call("12wOHk8ywb0", {active_id, 1}) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(call("4DM06U2BNEY", {idle_id, 0}) == 0);
    CHECK(call("R1Jvn8bSCW8", {active_id}) == 0);
    idle.join();
    unsatisfied.join();
    CHECK(results[0] == u32(ORBIS_KERNEL_ERROR_ECANCELED));
    CHECK(results[1] == u32(ORBIS_KERNEL_ERROR_EACCES));
    std::printf("UNRELATED_SEMAPHORE_SWITCHES unrelated=%ld unsatisfied=%ld signals=32\n",
                switches[0], switches[1]);
    // Allow scheduler noise/spurious wakes, but reject a wake per signal.
    CHECK(switches[0] >= 0 && switches[0] < 8);
    CHECK(switches[1] >= 0 && switches[1] < 8);
    CHECK(call("R1Jvn8bSCW8", {idle_id}) == 0);
#endif
    // Concurrent signal/Stop must preserve waiter lifetime and token ownership.
    for (unsigned n = 0; n < 64; ++n) {
        CHECK(call("188x57JYp0g", {base, base + 0x100, 1, 0, 1, 0}) == 0);
        const u32 raced_id = get.operator()<u32>(base);
        std::jthread waiter([&](std::stop_token stop) {
            first = sem.Dispatch("Zxa0VhQVTsk", {raced_id, 1, 0}, 700, stop);
        });
        entered(raced_id, 1);
        std::jthread signal([&] { (void)call("4czppHBiriw", {raced_id, 1}); });
        waiter.request_stop();
        waiter.join();
        signal.join();
        CHECK(first == 0 || first == u32(ORBIS_KERNEL_ERROR_EINTR));
        entered(raced_id, 0);
        const auto leftover = call("12wOHk8ywb0", {raced_id, 1});
        CHECK(leftover == (first == 0 ? u32(ORBIS_KERNEL_ERROR_EBUSY) : 0));
        CHECK(call("R1Jvn8bSCW8", {raced_id}) == 0);
    }
    GuestKernelSemaphore foreground(*space);
    foreground.AdmitForeground();
    put(base + 0x200, std::array<char, 17>{'S', 'u', 's', 'p', 'e', 'n', 'd',
                                            'S', 'e', 'm', 'a', 'p', 'h', 'o', 'r', 'e', 0});
    auto foreground_call = [&](std::string_view n, std::array<u64, 6> a = {}) {
        return foreground.Dispatch(n, a, 700);
    };
    CHECK(foreground_call("188x57JYp0g", {base + 0x300, base + 0x200, 1, 0, 1, 0}) == 0);
    const u32 foreground_id = [&] {
        u32 value{};
        CHECK(space->Read(GuestAddress{base + 0x300}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    }();
    CHECK(foreground_call("Zxa0VhQVTsk", {foreground_id, 1, 0}) == 0);
    put(base + 0x240, std::array<char, 17>{'R', 'e', 's', 'u', 'm', 'e',
                                            'S', 'e', 'm', 'a', 'p', 'h', 'o', 'r', 'e', 0});
    CHECK(foreground_call("188x57JYp0g", {base + 0x300, base + 0x240, 1, 0, 1, 0}) == 0);
    const u32 resume_id = [&] {
        u32 value{};
        CHECK(space->Read(GuestAddress{base + 0x300}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    }();
    CHECK(foreground_call("Zxa0VhQVTsk", {resume_id, 1, 0}) == 0);
    std::printf("GUEST_KERNEL_SEMAPHORE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
