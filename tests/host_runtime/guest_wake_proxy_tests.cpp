// SPDX-License-Identifier: GPL-2.0-or-later
// Wake proxy (core/host_runtime/guest_wake_proxy): every posted notify reaches its waiter, with
// posts spaced around the proxy's spin window so that it keeps falling asleep and being woken.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "core/host_runtime/guest_wake_proxy.h"

using namespace Core::HostRuntime;
using Clock = std::chrono::steady_clock;

static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)

namespace {

// Same shape as the guest synchronization waiters: state published under the waiter's own lock,
// notify after releasing it.
struct Waiter {
    std::mutex park;
    std::condition_variable parked;
    unsigned long posted{};          // under park
    unsigned long consumed{};        // under park
    Clock::time_point oldest_unseen; // under park: when posted last moved ahead of consumed
};

void NotifyWaiter(void* waiter) {
    static_cast<Waiter*>(waiter)->parked.notify_one();
}

unsigned long StatusValue(const std::string& status, const char* key) {
    const std::string field = std::string(key) + ": ";
    const auto at = status.find(field);
    return at == std::string::npos ? 0 : std::stoul(status.substr(at + field.size()));
}

// Producers bump a random waiter's `posted` and post its notify; each waiter consumes whatever was
// posted to it. A waiter whose wait times out while a post older than `stall / 2` is unconsumed
// has lost a wake (the proxy issues notifies within microseconds).
bool Run(unsigned producers, unsigned waiters, unsigned posts_per_producer, unsigned max_gap_us,
         std::chrono::milliseconds stall) {
    std::vector<std::shared_ptr<Waiter>> targets;
    for (unsigned i = 0; i < waiters; ++i) {
        targets.push_back(std::make_shared<Waiter>());
    }
    std::atomic<bool> done{false};
    std::atomic<unsigned long> lost{0}, direct{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < waiters; ++i) {
        threads.emplace_back([&, waiter = targets[i]] {
            std::unique_lock lock(waiter->park);
            for (;;) {
                if (waiter->posted != waiter->consumed) {
                    waiter->consumed = waiter->posted;
                    continue;
                }
                if (done.load()) {
                    return;
                }
                if (waiter->parked.wait_for(lock, stall) == std::cv_status::timeout &&
                    waiter->posted != waiter->consumed &&
                    Clock::now() - waiter->oldest_unseen >= stall / 2) {
                    lost.fetch_add(1);
                }
            }
        });
    }
    std::vector<std::thread> posters;
    for (unsigned p = 0; p < producers; ++p) {
        posters.emplace_back([&, p] {
            std::mt19937 random(1234 + p);
            std::uniform_int_distribution<unsigned> pick(0, waiters - 1);
            std::uniform_int_distribution<unsigned> gap(0, max_gap_us);
            for (unsigned n = 0; n < posts_per_producer; ++n) {
                const auto& waiter = targets[pick(random)];
                {
                    std::lock_guard lock(waiter->park);
                    if (waiter->posted == waiter->consumed) {
                        waiter->oldest_unseen = Clock::now();
                    }
                    ++waiter->posted;
                }
                if (!WakeProxy::Post(&NotifyWaiter, waiter)) {
                    direct.fetch_add(1);
                    waiter->parked.notify_one();
                }
                const unsigned us = gap(random);
                if (us != 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(us));
                }
            }
        });
    }
    for (auto& poster : posters) {
        poster.join();
    }
    // Every waiter must catch up on its own.
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    bool caught_up = false;
    while (!caught_up && Clock::now() < deadline) {
        caught_up = true;
        for (auto& waiter : targets) {
            std::lock_guard lock(waiter->park);
            caught_up = caught_up && waiter->posted == waiter->consumed;
        }
        if (!caught_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    done.store(true);
    for (auto& waiter : targets) {
        std::lock_guard lock(waiter->park);
        waiter->parked.notify_one();
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::printf("producers=%u waiters=%u posts=%u max_gap_us=%u direct=%lu caught_up=%d lost=%lu\n",
                producers, waiters, producers * posts_per_producer, max_gap_us, direct.load(),
                caught_up ? 1 : 0, lost.load());
    return caught_up && lost.load() == 0;
}

} // namespace

int main() {
    // Off: Post declines and callers notify themselves.
    CHECK(WakeProxy::Command({"off"}).find("wake_proxy: off") != std::string::npos);
    {
        auto waiter = std::make_shared<Waiter>();
        CHECK(!WakeProxy::Post(&NotifyWaiter, waiter));
    }
    CHECK(WakeProxy::Command({"on"}).find("wake_proxy: on") != std::string::npos);
    const auto before = WakeProxy::Command({"status"});

    // Default spin window, posts both inside and beyond it.
    CHECK(Run(4, 8, 20000, 400, std::chrono::milliseconds(200)));
    // No spin window: the proxy sleeps after every batch, so posts keep racing with its sleep.
    CHECK(WakeProxy::Command({"spin", "0"}).find("spin_us: 0") != std::string::npos);
    CHECK(Run(4, 8, 20000, 50, std::chrono::milliseconds(200)));
    // Bursts without gaps, more producers and waiters.
    CHECK(WakeProxy::Command({"spin", "200"}).find("spin_us: 200") != std::string::npos);
    CHECK(Run(8, 16, 20000, 0, std::chrono::milliseconds(200)));

    // A waiter can catch up through a later post's notify while the proxy is still issuing an
    // earlier one: let the queue drain.
    std::string after;
    unsigned long posts{}, notifies{};
    for (int i = 0; i < 2000; ++i) {
        after = WakeProxy::Command({"status"});
        posts = StatusValue(after, "posts") - StatusValue(before, "posts");
        notifies = StatusValue(after, "notifies") - StatusValue(before, "notifies");
        if (notifies == posts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("proxy posts=%lu notifies=%lu kicks=%lu sleeps=%lu full=%lu\n", posts, notifies,
                StatusValue(after, "kicks") - StatusValue(before, "kicks"),
                StatusValue(after, "sleeps") - StatusValue(before, "sleeps"),
                StatusValue(after, "full") - StatusValue(before, "full"));
    CHECK(posts > 0);
    // The waiters caught up, which needs every queued notify, so all have been issued.
    CHECK(notifies == posts);
    CHECK(StatusValue(after, "sleeps") > StatusValue(before, "sleeps"));

    WakeProxy::Command({"off"});
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
