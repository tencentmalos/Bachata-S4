// SPDX-License-Identifier: GPL-2.0-or-later
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <future>
#include "core/host_runtime/guest_semaphore.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace std::chrono_literals;
unsigned checks{};
void Check(bool ok) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL check %u\n", checks);
        std::_Exit(1);
    }
}
int main() {
    auto made = GuestAddressSpace::Create({.reservation_size = 16ULL << 20});
    Check(bool(made));
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    Check(bool(space->Map({GuestAddress{base}, HostPageSize() * 4},
                          GuestPermission::Read | GuestPermission::Write)));
    u64 allocation = base + HostPageSize();
    GuestSemaphoreDomain sem(*space, [&] { return allocation += 64; });
    for (unsigned round = 0; round < 64; ++round) {
        Check(sem.Init(base, 0, 0) == 0);
        std::stop_source cancel;
        auto a = std::async(std::launch::async, [&] {
            return sem.Wait(base, false, cancel.get_token(), std::chrono::steady_clock::now() + 2s);
        });
        auto b = std::async(std::launch::async, [&] {
            return sem.Wait(base, false, {}, std::chrono::steady_clock::now() + 2s);
        });
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (sem.Pending(base) != 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        Check(sem.Pending(base) == 2 && sem.Destroy(base) == POSIX_EBUSY);
        std::barrier race(3);
        std::jthread signal([&] {
            race.arrive_and_wait();
            if (sem.Post(base) != 0)
                std::_Exit(2);
        });
        std::jthread stop([&] {
            race.arrive_and_wait();
            cancel.request_stop();
        });
        race.arrive_and_wait();
        signal.join();
        stop.join();
        Check(a.wait_for(2s) == std::future_status::ready);
        const auto result = a.get();
        Check(result == 0 || result == POSIX_EINTR);
        if (result == 0)
            Check(sem.Post(base) == 0);
        Check(b.wait_for(2s) == std::future_status::ready && b.get() == 0);
        Check(sem.Pending(base) == 0 && sem.Wait(base, true, {}) == POSIX_EAGAIN);
        Check(sem.Destroy(base) == 0);
    }
    Check(space->Counts().live_pins == 0);
    std::printf("SYNC_CANCEL_RELAY checks=%u failures=0 rounds=64\n", checks);
}
