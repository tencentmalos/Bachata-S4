// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Platform-neutral tests of the shared kernel synchronization core
// (src/core/libraries/kernel/sync). Built on desktop and for the Android host.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/libraries/kernel/sync/condition_variable.h"
#include "core/libraries/kernel/sync/counting_semaphore.h"
#include "core/libraries/kernel/sync/event_flags.h"
#include "core/libraries/kernel/sync/kernel_semaphore.h"
#include "core/libraries/kernel/sync/lock_word.h"
#include "core/libraries/kernel/sync/object_table.h"
#include "core/libraries/kernel/sync/rw_lock.h"

using namespace Libraries::Kernel;
using namespace std::chrono_literals;

static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> failures{0};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)

template <class Pred>
static bool Eventually(Pred&& pred) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(100us);
    }
    return true;
}

static void KernelSemaphoreTests() {
    using Sync::KernelSemaphore;
    {
        KernelSemaphore sem{"basic", 0, 3, true};
        CHECK(sem.Poll(1) == ORBIS_KERNEL_ERROR_EBUSY);
        CHECK(sem.Poll(0) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sem.Poll(4) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sem.Signal(0) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sem.Signal(4) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sem.Signal(3) == ORBIS_OK);
        CHECK(sem.Signal(1) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sem.Poll(2) == ORBIS_OK);
        CHECK(sem.Poll(2) == ORBIS_KERNEL_ERROR_EBUSY);
        CHECK(sem.Poll(1) == ORBIS_OK);
        u32 zero = 0;
        bool parked = true;
        CHECK(sem.Wait(1, 0, &zero, {}, &parked) == ORBIS_KERNEL_ERROR_ETIMEDOUT);
        CHECK(!parked);
        u32 budget = 1000;
        CHECK(sem.Wait(1, 0, &budget, {}, &parked) == ORBIS_KERNEL_ERROR_ETIMEDOUT);
        CHECK(parked && budget == 0);
        CHECK(sem.Wait(4, 0, nullptr) == ORBIS_KERNEL_ERROR_EINVAL);
    }
    {
        // Signal skips an unsatisfiable first waiter and serves the next one.
        KernelSemaphore sem{"skip", 0, 3, true};
        s32 first = -1, second = -1;
        std::thread w1([&] { first = sem.Wait(2, 0, nullptr); });
        CHECK(Eventually([&] { return sem.Waiting() == 1; }));
        std::thread w2([&] { second = sem.Wait(1, 0, nullptr); });
        CHECK(Eventually([&] { return sem.Waiting() == 2; }));
        CHECK(sem.Signal(1) == ORBIS_OK);
        w2.join();
        CHECK(second == ORBIS_OK);
        CHECK(sem.Waiting() == 1);
        s32 waiters = -1;
        CHECK(sem.Cancel(4, &waiters) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sem.Cancel(0, &waiters) == ORBIS_OK);
        w1.join();
        CHECK(waiters == 1);
        CHECK(first == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sem.Snapshot().value == 0);
        CHECK(sem.Cancel(-1, nullptr) == ORBIS_OK);
        CHECK(sem.Snapshot().value == 0); // back to the initial count
    }
    {
        // Priority order: the lower priority value is served first.
        KernelSemaphore sem{"prio", 0, 3, false};
        s32 low = -1, high = -1;
        std::thread w1([&] { low = sem.Wait(1, 700, nullptr); });
        CHECK(Eventually([&] { return sem.Waiting() == 1; }));
        std::thread w2([&] { high = sem.Wait(1, 500, nullptr); });
        CHECK(Eventually([&] { return sem.Waiting() == 2; }));
        CHECK(sem.Signal(1) == ORBIS_OK);
        w2.join();
        CHECK(high == ORBIS_OK);
        CHECK(sem.Waiting() == 1);
        sem.Delete();
        w1.join();
        CHECK(low == ORBIS_KERNEL_ERROR_EACCES);
    }
    {
        // Timed wait satisfied in time reports the time left.
        KernelSemaphore sem{"timed", 0, 1, true};
        u32 budget = 2'000'000;
        s32 result = -1;
        bool parked = false;
        std::thread w([&] { result = sem.Wait(1, 0, &budget, {}, &parked); });
        CHECK(Eventually([&] { return sem.Waiting() == 1; }));
        CHECK(sem.Signal(1) == ORBIS_OK);
        w.join();
        CHECK(result == ORBIS_OK && parked);
        CHECK(budget > 0 && budget <= 2'000'000);
    }
    {
        // A stop request interrupts a wait; a signal racing it keeps token ownership exact.
        for (int round = 0; round < 64; ++round) {
            KernelSemaphore sem{"race", 0, 1, true};
            std::stop_source stop;
            s32 result = -1;
            std::thread w([&] { result = sem.Wait(1, 0, nullptr, stop.get_token()); });
            CHECK(Eventually([&] { return sem.Waiting() == 1; }));
            std::thread signal([&] { (void)sem.Signal(1); });
            stop.request_stop();
            w.join();
            signal.join();
            CHECK(result == ORBIS_OK || result == ORBIS_KERNEL_ERROR_EINTR);
            CHECK(sem.Waiting() == 0);
            // The token was either taken by the waiter or is still there.
            CHECK(sem.Poll(1) == (result == ORBIS_OK ? ORBIS_KERNEL_ERROR_EBUSY : ORBIS_OK));
        }
    }
    {
        // Many producers and consumers: every token is consumed exactly once.
        KernelSemaphore sem{"stress", 0, 1000, true};
        std::atomic<int> consumed{0};
        std::vector<std::thread> threads;
        constexpr int PerThread = 2000;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < PerThread; ++i) {
                    if (sem.Wait(1, 0, nullptr) == ORBIS_OK) {
                        consumed.fetch_add(1);
                    }
                }
            });
        }
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < PerThread; ++i) {
                    while (sem.Signal(1) != ORBIS_OK) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(consumed.load() == 4 * PerThread);
        CHECK(sem.Snapshot().value == 0);
    }
    {
        Sync::ObjectTable<KernelSemaphore> table{2};
        const auto a = table.Insert(std::make_shared<KernelSemaphore>("a", 0, 1, true));
        const auto b = table.Insert(std::make_shared<KernelSemaphore>("b", 0, 1, true));
        CHECK(a && b && *a != *b && *a != 0);
        CHECK(!table.Insert(std::make_shared<KernelSemaphore>("c", 0, 1, true)));
        CHECK(table.Find(*a) && table.Find(*a)->Name() == "a");
        CHECK(table.Erase(*a) != nullptr);
        CHECK(!table.Find(*a) && !table.Erase(*a));
        const auto c = table.Insert(std::make_shared<KernelSemaphore>("c", 0, 1, true));
        CHECK(c && *c != *a && *c != *b);
    }
}

static void CountingSemaphoreTests() {
    using Sync::CountingSemaphore;
    using Sync::ParkerWait;
    {
        CountingSemaphore sem{0};
        CHECK(!sem.TryWait());
        CHECK(sem.Post() == 0);
        CHECK(sem.Value() == 1);
        CHECK(sem.TryWait());
        CHECK(!sem.TryWait());
        ParkerWait timed{{}, std::chrono::steady_clock::now()};
        CHECK(sem.Wait(timed) == POSIX_ETIMEDOUT);
        CHECK(sem.Waiting() == 0);
        CountingSemaphore full{CountingSemaphore::MaxValue};
        CHECK(full.Post() == POSIX_EOVERFLOW);
    }
    {
        // A blocked waiter is woken by a post; a stop interrupts another.
        CountingSemaphore sem{0};
        int result = -1;
        std::thread w([&] {
            ParkerWait wait{{}};
            result = sem.Wait(wait);
        });
        CHECK(Eventually([&] { return sem.Waiting() == 1; }));
        CHECK(sem.Post() == 0);
        w.join();
        CHECK(result == 0 && sem.Value() == 0);
        std::stop_source stop;
        std::thread c([&] {
            ParkerWait wait{stop.get_token()};
            result = sem.Wait(wait);
        });
        CHECK(Eventually([&] { return sem.Waiting() == 1; }));
        stop.request_stop();
        c.join();
        CHECK(result == POSIX_EINTR && sem.Waiting() == 0);
    }
    {
        // One post wakes one waiter: the other stays parked.
        CountingSemaphore sem{0};
        std::atomic<int> done{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 2; ++i) {
            threads.emplace_back([&] {
                ParkerWait wait{{}};
                if (sem.Wait(wait) == 0) {
                    done.fetch_add(1);
                }
            });
        }
        CHECK(Eventually([&] { return sem.Waiting() == 2; }));
        CHECK(sem.Post() == 0);
        CHECK(Eventually([&] { return done.load() == 1; }));
        std::this_thread::sleep_for(20ms);
        CHECK(done.load() == 1 && sem.Waiting() == 1);
        CHECK(sem.Post() == 0);
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(done.load() == 2 && sem.Value() == 0);
    }
    {
        // A selected waiter that is stopped without taking its token passes the wake on.
        for (int round = 0; round < 64; ++round) {
            CountingSemaphore sem{0};
            std::stop_source stop;
            int first = -1, second = -1;
            std::thread a([&] {
                ParkerWait wait{stop.get_token()};
                first = sem.Wait(wait);
            });
            CHECK(Eventually([&] { return sem.Waiting() == 1; }));
            std::thread b([&] {
                ParkerWait wait{{}, std::chrono::steady_clock::now() + 5s};
                second = sem.Wait(wait);
            });
            CHECK(Eventually([&] { return sem.Waiting() == 2; }));
            std::thread post([&] { (void)sem.Post(); });
            stop.request_stop();
            a.join();
            post.join();
            b.join();
            // Exactly one of them took the single token.
            CHECK((first == 0) != (second == 0));
            CHECK(first == 0 || first == POSIX_EINTR);
            CHECK(sem.Value() == 0 && sem.Waiting() == 0);
        }
    }
    {
        // Producers and consumers with trywait stealing: every post is consumed once.
        CountingSemaphore sem{0};
        std::atomic<int> consumed{0};
        std::vector<std::thread> threads;
        constexpr int PerThread = 5000;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < PerThread; ++i) {
                    if (t % 2 == 0) {
                        ParkerWait wait{{}};
                        if (sem.Wait(wait) == 0) {
                            consumed.fetch_add(1);
                        }
                    } else {
                        while (!sem.TryWait()) {
                            std::this_thread::yield();
                        }
                        consumed.fetch_add(1);
                    }
                }
            });
        }
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 2 * PerThread; ++i) {
                    CHECK(sem.Post() == 0);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(consumed.load() == 4 * PerThread);
        CHECK(sem.Value() == 0 && sem.Waiting() == 0);
    }
}

static void RwLockTests() {
    using Sync::ParkerWait;
    using Sync::RwLock;
    const auto lock_now = [](RwLock& rw, u64 owner, bool write) {
        ParkerWait wait{{}};
        return rw.Lock(owner, write, false, wait);
    };
    const auto try_lock = [](RwLock& rw, u64 owner, bool write) {
        ParkerWait wait{{}};
        return rw.Lock(owner, write, true, wait);
    };
    {
        RwLock rw{0};
        CHECK(lock_now(rw, 1, true) == 0);
        CHECK(lock_now(rw, 1, true) == POSIX_EDEADLK);
        CHECK(try_lock(rw, 1, true) == POSIX_EBUSY);
        CHECK(lock_now(rw, 1, false) == POSIX_EDEADLK);
        CHECK(try_lock(rw, 2, false) == POSIX_EBUSY);
        CHECK(rw.Unlock(2) == POSIX_EPERM);
        CHECK(rw.Busy());
        CHECK(rw.Unlock(1) == 0);
        CHECK(!rw.Busy());
        CHECK(rw.Unlock(1) == POSIX_EPERM);
        // Recursive reads by one owner, reads by others, a writer refused meanwhile.
        CHECK(lock_now(rw, 1, false) == 0);
        CHECK(lock_now(rw, 1, false) == 0);
        CHECK(try_lock(rw, 2, false) == 0);
        CHECK(lock_now(rw, 1, true) == POSIX_EDEADLK);
        CHECK(try_lock(rw, 3, true) == POSIX_EBUSY);
        CHECK(rw.Unlock(1) == 0 && rw.Unlock(1) == 0 && rw.Unlock(2) == 0);
        CHECK(rw.Unlock(1) == POSIX_EPERM);
        CHECK(!rw.Busy());
    }
    {
        // Type 2 refuses a recursive read.
        RwLock rw{2};
        CHECK(lock_now(rw, 1, false) == 0);
        CHECK(lock_now(rw, 1, false) == POSIX_EDEADLK);
        CHECK(try_lock(rw, 1, false) == POSIX_EBUSY);
        CHECK(rw.Unlock(1) == 0);
    }
    for (u32 type : {0u, 1u}) {
        // A waiting writer blocks new readers only for the writer-preferring types; a thread
        // that already reads may always read again.
        RwLock rw{type};
        CHECK(lock_now(rw, 1, false) == 0);
        int writer = -1;
        std::thread w([&] { writer = lock_now(rw, 2, true); });
        CHECK(Eventually([&] { return rw.Waiting() == 1; }));
        CHECK(try_lock(rw, 3, false) == (type == 0 ? 0 : POSIX_EBUSY));
        if (type == 0) {
            CHECK(rw.Unlock(3) == 0);
        }
        CHECK(lock_now(rw, 1, false) == 0); // recursion by an existing reader
        CHECK(rw.Unlock(1) == 0);
        CHECK(rw.Unlock(1) == 0);
        w.join();
        CHECK(writer == 0);
        // Readers queued behind the writer all enter when it leaves.
        std::atomic<int> readers{0};
        std::vector<std::thread> threads;
        for (u64 owner = 10; owner < 14; ++owner) {
            threads.emplace_back([&, owner] {
                if (lock_now(rw, owner, false) == 0) {
                    readers.fetch_add(1);
                }
            });
        }
        CHECK(Eventually([&] { return rw.Waiting() == 4; }));
        CHECK(rw.Unlock(2) == 0);
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(readers.load() == 4);
        for (u64 owner = 10; owner < 14; ++owner) {
            CHECK(rw.Unlock(owner) == 0);
        }
        CHECK(!rw.Busy());
    }
    {
        // Timeout and stop leave no trace, and a departing writer lets readers in.
        RwLock rw{1};
        CHECK(lock_now(rw, 1, false) == 0);
        ParkerWait timed{{}, std::chrono::steady_clock::now() + 5ms};
        CHECK(rw.Lock(2, true, false, timed) == POSIX_ETIMEDOUT);
        std::stop_source stop;
        int writer = -1;
        std::thread w([&] {
            ParkerWait wait{stop.get_token()};
            writer = rw.Lock(2, true, false, wait);
        });
        CHECK(Eventually([&] { return rw.Waiting() == 1; }));
        int reader = -1;
        std::thread r([&] { reader = lock_now(rw, 3, false); });
        CHECK(Eventually([&] { return rw.Waiting() == 2; }));
        stop.request_stop();
        w.join();
        r.join();
        CHECK(writer == POSIX_EINTR && reader == 0);
        CHECK(rw.Unlock(3) == 0 && rw.Unlock(1) == 0 && !rw.Busy());
    }
    {
        // Mutual exclusion under contention for every type.
        for (u32 type : {0u, 1u, 2u}) {
            RwLock rw{type};
            std::atomic<int> inside_writers{0}, inside_readers{0};
            std::atomic<bool> violated{false};
            std::vector<std::thread> threads;
            for (u64 owner = 1; owner <= 6; ++owner) {
                threads.emplace_back([&, owner] {
                    for (int i = 0; i < 1500; ++i) {
                        const bool write = (i + owner) % 3 == 0;
                        if (lock_now(rw, owner, write) != 0) {
                            violated = true;
                            continue;
                        }
                        if (write) {
                            if (inside_writers.fetch_add(1) != 0 || inside_readers.load() != 0) {
                                violated = true;
                            }
                            inside_writers.fetch_sub(1);
                        } else {
                            inside_readers.fetch_add(1);
                            if (inside_writers.load() != 0) {
                                violated = true;
                            }
                            inside_readers.fetch_sub(1);
                        }
                        if (rw.Unlock(owner) != 0) {
                            violated = true;
                        }
                    }
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            CHECK(!violated.load());
            CHECK(!rw.Busy());
        }
    }
}

// A mutex with an owner, standing in for the guest mutex a platform hands over.
struct TestMutex {
    std::mutex mutex;
    std::atomic<u64> owner{0};
    void Lock(u64 self) {
        mutex.lock();
        owner = self;
    }
    void Unlock() {
        owner = 0;
        mutex.unlock();
    }
};

struct TestCondWait : Sync::ParkerWait {
    TestCondWait(TestMutex& m_, u64 self_, std::stop_token stop, Sync::Deadline deadline = {})
        : ParkerWait{std::move(stop), deadline}, m{m_}, self{self_} {}
    u64 Owner() const {
        return self;
    }
    const void* Context() const {
        return &m;
    }
    int ReleaseMutex() {
        if (m.owner.load() != self) {
            return POSIX_EPERM;
        }
        m.Unlock();
        return 0;
    }
    int ReacquireMutex() {
        m.Lock(self);
        return 0;
    }
    TestMutex& m;
    u64 self;
};

static void ConditionVariableTests() {
    using Sync::ConditionVariable;
    {
        ConditionVariable cv;
        TestMutex m;
        TestCondWait not_owner{m, 1, {}};
        CHECK(cv.Wait(not_owner) == POSIX_EPERM);
        m.Lock(1);
        TestCondWait timed{m, 1, {}, std::chrono::steady_clock::now() + 2ms};
        CHECK(cv.Wait(timed) == POSIX_ETIMEDOUT);
        CHECK(m.owner.load() == 1 && !cv.Busy());
        m.Unlock();
        CHECK(!cv.Signal());
    }
    {
        // Signal wakes one waiter, broadcast the rest; each returns holding the mutex.
        ConditionVariable cv;
        TestMutex m;
        std::atomic<int> woken{0};
        std::vector<std::thread> threads;
        for (u64 self = 1; self <= 3; ++self) {
            threads.emplace_back([&, self] {
                m.Lock(self);
                TestCondWait wait{m, self, {}};
                if (cv.Wait(wait) == 0 && m.owner.load() == self) {
                    woken.fetch_add(1);
                }
                m.Unlock();
            });
        }
        CHECK(Eventually([&] { return cv.Queued() == 3; }));
        CHECK(cv.Signal());
        CHECK(Eventually([&] { return woken.load() == 1; }));
        std::this_thread::sleep_for(10ms);
        CHECK(woken.load() == 1 && cv.Queued() == 2);
        cv.Broadcast();
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(woken.load() == 3 && !cv.Busy());
    }
    {
        // A targeted signal picks that owner only.
        ConditionVariable cv;
        TestMutex m;
        int a = -1, b = -1;
        std::thread ta([&] {
            m.Lock(1);
            TestCondWait wait{m, 1, {}};
            a = cv.Wait(wait);
            m.Unlock();
        });
        CHECK(Eventually([&] { return cv.Queued() == 1; }));
        std::thread tb([&] {
            m.Lock(2);
            TestCondWait wait{m, 2, {}};
            b = cv.Wait(wait);
            m.Unlock();
        });
        CHECK(Eventually([&] { return cv.Queued() == 2; }));
        CHECK(!cv.Signal(3));
        CHECK(cv.Signal(2));
        tb.join();
        CHECK(b == 0 && cv.Queued() == 1);
        CHECK(cv.Signal(1));
        ta.join();
        CHECK(a == 0 && !cv.Busy());
    }
    {
        // A timed-out waiter keeps the condition busy until it has its mutex back; a stop
        // interrupts a wait.
        ConditionVariable cv;
        TestMutex m;
        int result = -1;
        std::thread t([&] {
            m.Lock(1);
            TestCondWait wait{m, 1, {}, std::chrono::steady_clock::now() + 200ms};
            result = cv.Wait(wait);
            m.Unlock();
        });
        CHECK(Eventually([&] { return cv.Queued() == 1; }));
        m.Lock(2);
        CHECK(Eventually([&] { return cv.Reacquiring() == 1; }));
        CHECK(cv.Busy());
        CHECK(!cv.Signal(1)); // no longer eligible
        m.Unlock();
        t.join();
        CHECK(result == POSIX_ETIMEDOUT && !cv.Busy());
        std::stop_source stop;
        std::thread s([&] {
            m.Lock(3);
            TestCondWait wait{m, 3, stop.get_token()};
            result = cv.Wait(wait);
            m.Unlock();
        });
        CHECK(Eventually([&] { return cv.Queued() == 1; }));
        stop.request_stop();
        s.join();
        CHECK(result == POSIX_EINTR && !cv.Busy());
    }
    {
        // A deferred wake is issued by the platform later.
        ConditionVariable cv;
        TestMutex m;
        int result = -1;
        std::thread t([&] {
            m.Lock(1);
            TestCondWait wait{m, 1, {}};
            result = cv.Wait(wait);
            m.Unlock();
        });
        CHECK(Eventually([&] { return cv.Queued() == 1; }));
        std::shared_ptr<Sync::WaitSlot> kept;
        CHECK(cv.Signal(0, [&](const ConditionVariable::Waiter& waiter) {
            kept = waiter.slot;
            return true;
        }));
        std::this_thread::sleep_for(10ms);
        CHECK(result == -1 && kept);
        kept->Unpark();
        t.join();
        CHECK(result == 0 && !cv.Busy());
    }
    {
        // A thread that takes the mutex the moment the waiter releases it, and signals, wakes
        // that waiter: it is counted before the release, so the lock-free empty check in
        // Signal cannot miss it. The hook lets the signaller run inside that window.
        struct RacingWait : TestCondWait {
            using TestCondWait::TestCondWait;
            std::atomic<int>* signalled{};
            int ReleaseMutex() {
                const int error = TestCondWait::ReleaseMutex();
                // The signaller holds the mutex next; give it the chance to signal before this
                // waiter goes on (it cannot finish while it has to wait for the queue lock).
                const auto end = std::chrono::steady_clock::now() + 100ms;
                while (signalled->load() == 0 && std::chrono::steady_clock::now() < end) {
                    std::this_thread::yield();
                }
                return error;
            }
        };
        ConditionVariable cv;
        TestMutex m;
        std::atomic<int> signalled{0};
        int result = -1;
        m.Lock(1);
        std::thread signaller([&] {
            m.Lock(2); // granted by the waiter's release
            cv.Signal();
            signalled = 1;
            m.Unlock();
        });
        RacingWait wait{m, 1, {}, std::chrono::steady_clock::now() + 2s};
        wait.signalled = &signalled;
        result = cv.Wait(wait);
        m.Unlock();
        signaller.join();
        CHECK(result == 0);
        CHECK(!cv.Busy());
    }
    {
        // Producer/consumer with a predicate: no lost wakeups.
        ConditionVariable cv;
        TestMutex m;
        int items = 0, consumed = 0;
        constexpr int Total = 20000;
        std::vector<std::thread> threads;
        for (u64 self = 1; self <= 3; ++self) {
            threads.emplace_back([&, self] {
                for (;;) {
                    m.Lock(self);
                    while (items == 0 && consumed < Total) {
                        TestCondWait wait{m, self, {}};
                        (void)cv.Wait(wait);
                    }
                    if (consumed >= Total) {
                        m.Unlock();
                        cv.Broadcast();
                        return;
                    }
                    --items;
                    ++consumed;
                    m.Unlock();
                }
            });
        }
        for (int i = 0; i < Total; ++i) {
            m.Lock(100);
            ++items;
            m.Unlock();
            cv.Signal();
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(consumed == Total && items == 0 && !cv.Busy());
    }
}

static void LockWordTests() {
    using Sync::LockWord;
    {
        LockWord word;
        CHECK(word.try_lock());
        CHECK(!word.try_lock());
        const auto begin = std::chrono::steady_clock::now();
        CHECK(!word.try_lock_for(5ms));
        CHECK(std::chrono::steady_clock::now() - begin >= 4ms);
        std::thread releaser([&] {
            std::this_thread::sleep_for(5ms);
            word.unlock();
        });
        CHECK(word.try_lock_for(2s));
        releaser.join();
        word.unlock();
        CHECK(word.try_lock_until(std::chrono::steady_clock::now()));
        word.unlock();
    }
    {
        // Mutual exclusion and progress under contention.
        LockWord word;
        long counter = 0;
        std::vector<std::thread> threads;
        for (int t = 0; t < 6; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 20000; ++i) {
                    word.lock();
                    ++counter;
                    word.unlock();
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(counter == 6 * 20000);
        CHECK(word.try_lock());
        word.unlock();
    }
}

static void EventFlagRuleTests() {
    using Sync::DecodeEventFlagAttributes;
    using Sync::DecodeEventFlagWaitMode;
    CHECK(DecodeEventFlagAttributes(0x00).has_value());
    CHECK(DecodeEventFlagAttributes(0x22)->queue_mode == EventFlagState::QueueMode::ThreadPrio);
    CHECK(DecodeEventFlagAttributes(0x22)->thread_mode == EventFlagState::ThreadMode::Multi);
    CHECK(DecodeEventFlagAttributes(0x11)->thread_mode == EventFlagState::ThreadMode::Single);
    CHECK(!DecodeEventFlagAttributes(0x03));
    CHECK(!DecodeEventFlagAttributes(0x30));
    CHECK(!DecodeEventFlagAttributes(0x100));
    CHECK(!DecodeEventFlagWaitMode(0x00));
    CHECK(!DecodeEventFlagWaitMode(0x03));
    CHECK(!DecodeEventFlagWaitMode(0x31));
    CHECK(!DecodeEventFlagWaitMode(0x41));
    CHECK(DecodeEventFlagWaitMode(0x01)->wait == EventFlagState::WaitMode::And);
    CHECK(DecodeEventFlagWaitMode(0x12)->wait == EventFlagState::WaitMode::Or);
    CHECK(DecodeEventFlagWaitMode(0x12)->clear == EventFlagState::ClearMode::All);
    CHECK(DecodeEventFlagWaitMode(0x21)->clear == EventFlagState::ClearMode::Bits);
}

static void ParkerTests() {
    using Sync::ParkResult;
    using Sync::Parker;
    {
        // An Unpark before Park is kept for it; the Park that returns Woken consumes it, so a
        // waiter that parks again sleeps (a woken rwlock or sem_t waiter that lost the race).
        Parker parker;
        parker.Unpark();
        CHECK(parker.Park({}) == ParkResult::Woken);
        CHECK(parker.Park(std::chrono::steady_clock::now() + 5ms) == ParkResult::TimedOut);
        std::thread t([&] {
            std::this_thread::sleep_for(5ms);
            parker.Unpark();
        });
        CHECK(parker.Park(std::chrono::steady_clock::now() + 2s) == ParkResult::Woken);
        t.join();
    }
    {
        // A woken writer that a reader overtakes (type 0) queues again and gets the lock when the
        // reader leaves.
        Sync::RwLock rw{0};
        Sync::ParkerWait none{{}};
        CHECK(rw.Lock(1, false, false, none) == 0);
        std::atomic<int> result{-1};
        std::thread writer([&] {
            Sync::ParkerWait wait{{}};
            result = rw.Lock(2, true, false, wait);
            if (result == 0) {
                rw.Unlock(2);
            }
        });
        CHECK(Eventually([&] { return rw.Waiting() == 1; }));
        CHECK(rw.Unlock(1) == 0);                      // selects the writer
        if (rw.Lock(3, false, true, none) == 0) {       // overtook it
            std::this_thread::sleep_for(20ms);
            CHECK(result.load() == -1 && rw.Waiting() == 1);
            CHECK(rw.Unlock(3) == 0);
        }
        writer.join();
        CHECK(result.load() == 0 && !rw.Busy());
    }
}

int main() {
    ParkerTests();
    EventFlagRuleTests();
    LockWordTests();
    KernelSemaphoreTests();
    CountingSemaphoreTests();
    RwLockTests();
    ConditionVariableTests();
    std::printf("SYNC_CORE checks=%u failures=%u\n", checks.load(), failures.load());
    std::fflush(stdout);
    return failures ? 1 : 0;
}
