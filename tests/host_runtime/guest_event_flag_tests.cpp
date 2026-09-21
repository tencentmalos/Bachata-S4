// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>
#include "core/libraries/kernel/threads/event_flag_state.h"
using namespace Libraries::Kernel;
using E = EventFlagState;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (0)
static void entered(E& e, size_t n) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (e.Waiting() != n && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    CHECK(e.Waiting() == n);
}
int main() {
    E e("jobs", E::ThreadMode::Multi, E::QueueMode::Fifo, 0xf);
    u64 result{};
    e.Clear(~u64{2});
    CHECK(e.Poll(2, E::WaitMode::And, E::ClearMode::None, &result) == ORBIS_KERNEL_ERROR_EBUSY);
    CHECK(result == 0xd);
    CHECK(e.Poll(5, E::WaitMode::And, E::ClearMode::Bits, &result) == 0 && result == 0xd);
    CHECK(e.Poll(8, E::WaitMode::Or, E::ClearMode::All, &result) == 0 && result == 8);
    u32 timeout = 1000;
    CHECK(e.Wait(1, E::WaitMode::And, E::ClearMode::None, &result, &timeout) == ORBIS_KERNEL_ERROR_ETIMEDOUT);
    CHECK(timeout == 0);
    auto wait = [&](E& flag, std::stop_token cancel = {}) {
        return flag.Wait(1, E::WaitMode::And, E::ClearMode::Bits, nullptr, nullptr, 700, cancel);
    };
    auto first = std::async(std::launch::async, [&] { return wait(e); }); entered(e, 1);
    auto second = std::async(std::launch::async, [&] { return wait(e); }); entered(e, 2);
    e.Set(1);
    CHECK(first.wait_for(2s) == std::future_status::ready && first.get() == 0);
    CHECK(second.wait_for(10ms) == std::future_status::timeout);
    int count{}; e.Cancel(4, &count);
    CHECK(count == 1 && second.get() == ORBIS_KERNEL_ERROR_ECANCELED);
    CHECK(e.Poll(4, E::WaitMode::And, E::ClearMode::All, &result) == 0 && result == 4);
    auto deleted = std::async(std::launch::async, [&] { return wait(e); }); entered(e, 1);
    e.Delete(); CHECK(deleted.get() == ORBIS_KERNEL_ERROR_EACCES);
    bool written = true;
    result = 0xfeed;
    CHECK(e.Poll(1, E::WaitMode::And, E::ClearMode::None, &result, &written) == ORBIS_KERNEL_ERROR_EACCES);
    CHECK(!written && result == 0xfeed);
    E single("single", E::ThreadMode::Single, E::QueueMode::Fifo, 0);
    auto one = std::async(std::launch::async, [&] { return wait(single); }); entered(single, 1);
    CHECK(wait(single) == ORBIS_KERNEL_ERROR_EPERM);
    written = true;
    CHECK(single.Wait(1, E::WaitMode::And, E::ClearMode::None, &result, nullptr, 0, {}, &written) == ORBIS_KERNEL_ERROR_EPERM);
    CHECK(!written && result == 0xfeed);
    single.Set(1); CHECK(one.get() == 0);
    // Same predicate/park protocol as production HLE: stop must wake every
    // owner without a Set, fake ready bit, timeout polling, or object deletion.
    for (unsigned round = 0; round < 64; ++round) {
        E jobs("cancel", E::ThreadMode::Multi, E::QueueMode::Fifo, 0);
        std::stop_source stop;
        std::vector<std::future<int>> pending;
        for (unsigned i = 0; i < 8; ++i)
            pending.push_back(std::async(std::launch::async, [&] { return wait(jobs, stop.get_token()); }));
        if (round & 1) entered(jobs, 8); // alternate parked and registration races
        stop.request_stop();
        for (auto& f : pending) {
            const bool ready = f.wait_for(2s) == std::future_status::ready;
            CHECK(ready);
            if (!ready) jobs.Delete(); // bounded failure cleanup, not production wake
            CHECK(f.get() == ORBIS_KERNEL_ERROR_ECANCELED);
        }
        CHECK(jobs.Waiting() == 0);
        jobs.Set(1); // cancellation must not leak into the next owner/token
        CHECK(wait(jobs) == 0);
    }
    std::printf("guest_event_flag_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
