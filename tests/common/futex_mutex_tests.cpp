// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <sys/mman.h>
#include <time.h>
#include "common/futex_mutex.h"
#include "common/spin_lock.h"

using namespace std::chrono_literals;
static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
        }                                                                                          \
    } while (0)
static Common::FutexMutex fault_lock;
static std::atomic<bool> fault_entered{}, fault_done{};
static void* watched;
static std::size_t page_size;
static void Fault(int, siginfo_t* info, void*) {
    if (info->si_addr != watched)
        _exit(90);
    fault_entered.store(true, std::memory_order_release);
    fault_lock.lock();
    if (mprotect(watched, page_size, PROT_READ | PROT_WRITE))
        _exit(91);
    fault_lock.unlock();
}
static std::atomic<unsigned> interruptions{};
static void Interrupt(int) {
    interruptions.fetch_add(1, std::memory_order_relaxed);
}
static double ThreadCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) * 1000.0 + t.tv_nsec / 1e6;
}
template <class Lock>
static double HeldOwnerCpu() {
    Lock lock;
    lock.lock();
    std::atomic<unsigned> ready{};
    std::array<double, 6> cpu{};
    std::array<std::thread, 6> threads;
    for (unsigned i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] {
            const auto start = ThreadCpuMs();
            ready.fetch_add(1);
            lock.lock();
            lock.unlock();
            cpu[i] = ThreadCpuMs() - start;
        });
    while (ready.load() != threads.size())
        std::this_thread::yield();
    std::this_thread::sleep_for(100ms);
    lock.unlock();
    double sum{};
    for (unsigned i = 0; i < threads.size(); ++i) {
        threads[i].join();
        sum += cpu[i];
    }
    return sum;
}
int main() {
    alarm(20); // Wake-chain failures must terminate instead of hanging CI/device.
    Common::FutexMutex m, independent;
    CHECK(m.try_lock());
    CHECK(!m.try_lock());
    m.unlock();
    CHECK(m.try_lock());
    m.unlock();
    std::uint64_t value{}, mirror{};
    std::atomic<bool> corrupt{};
    std::array<std::thread, 8> workers;
    for (auto& t : workers)
        t = std::thread([&] {
            for (unsigned i = 0; i < 20000; ++i) {
                std::scoped_lock lk(m);
                if (value != mirror)
                    corrupt = true;
                mirror = ++value;
            }
        });
    for (auto& t : workers)
        t.join();
    CHECK(value == 160000 && mirror == value && !corrupt);

    struct sigaction interrupt{};
    interrupt.sa_handler = Interrupt;
    sigemptyset(&interrupt.sa_mask);
    CHECK(sigaction(SIGUSR1, &interrupt, nullptr) == 0);
    m.lock();
    std::atomic<bool> waiting{}, acquired{}, independent_done{};
    std::thread waiter([&] {
        errno = EDOM;
        waiting = true;
        m.lock();
        acquired = (errno == EDOM);
        m.unlock();
    });
    while (!waiting.load())
        std::this_thread::yield();
    for (int i = 0; i < 10; ++i) {
        pthread_kill(waiter.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(1ms);
    }
    std::thread other([&] {
        std::scoped_lock lk(independent);
        independent_done = true;
    });
    other.join();
    CHECK(independent_done && !acquired && interruptions > 0);
    errno = ERANGE;
    m.unlock();
    CHECK(errno == ERANGE);
    waiter.join();
    CHECK(acquired);

    page_size = sysconf(_SC_PAGESIZE);
    watched = mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(watched != MAP_FAILED);
    struct sigaction action{}, old_action{};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGSEGV, &action, &old_action) == 0);
    fault_lock.lock();
    std::thread writer([] {
        errno = EBUSY;
        *static_cast<volatile unsigned char*>(watched) = 42;
        fault_done = (errno == EBUSY);
    });
    while (!fault_entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(10ms);
    CHECK(!fault_done);
    fault_lock.unlock();
    writer.join();
    CHECK(fault_done && *static_cast<unsigned char*>(watched) == 42);
    CHECK(sigaction(SIGSEGV, &old_action, nullptr) == 0);
    CHECK(munmap(watched, page_size) == 0);
    // Report CPU waste separately from correctness; no noisy speed threshold.
    const auto spin = HeldOwnerCpu<Common::SpinLock>();
    const auto futex = HeldOwnerCpu<Common::FutexMutex>();
    std::printf("checks=%d failures=%d increment_pairs=160000 spin_wait_cpu_ms=%.3f "
                "futex_wait_cpu_ms=%.3f\n",
                checks, failures, spin, futex);
    return failures != 0;
}
