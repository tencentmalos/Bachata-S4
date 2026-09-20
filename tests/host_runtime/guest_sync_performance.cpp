// SPDX-License-Identifier: GPL-2.0-or-later
// Same production domains on Linux x64 and Android arm64. No FEX in this layer.
// Native rows compare mechanisms; they do not pretend to implement the Orbis ABI.
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <semaphore.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include "common/futex_mutex.h"
#include "core/host_runtime/guest_kernel_semaphore.h"
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_rwlock.h"
#include "core/host_runtime/guest_semaphore.h"
#include "core/libraries/kernel/sync/mutex.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace std::chrono_literals;
namespace {
void Require(bool ok) {
    if (!ok) {
        std::puts("SYNC_BENCH_FAILURE");
        std::exit(2);
    }
}
u64 CpuNs() {
    timespec ts{};
    Require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
    return u64(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
struct Fixture {
    std::unique_ptr<GuestAddressSpace> space;
    u64 base{}, next{};
    Fixture() {
        auto result = GuestAddressSpace::Create({.reservation_size = 16ULL << 20});
        Require(bool(result));
        space = std::move(result).Value();
        base = space->ReservationBase().value;
        next = base + HostPageSize();
        Require(bool(space->Map({GuestAddress{base}, 8ULL << 20},
                                GuestPermission::Read | GuestPermission::Write)));
    }
    u64 Allocate() {
        const auto p = next;
        next += 64;
        return p;
    }
};
struct alignas(64) Lane {
    u64 count{}, cpu{};
    std::vector<double> samples;
};
template <class Acquire, class Release>
void Throughput(const char* name, unsigned round, unsigned threads, bool shared, Acquire acquire,
                Release release) {
    constexpr unsigned iterations = 4000;
    std::array<Lane, 12> lanes;
    std::barrier start(threads + 1), done(threads + 1);
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < threads; ++i)
        workers.emplace_back([&, i] {
            auto& lane = lanes[i];
            lane.samples.reserve(iterations / 64);
            start.arrive_and_wait();
            const auto cpu = CpuNs();
            for (unsigned n = 0; n < iterations; ++n) {
                const bool sample = n % 64 == 0;
                const auto begin = sample ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                const auto object = shared ? 0 : i;
                Require(acquire(object, i + 1) == 0);
                ++lanes[object].count;
                Require(release(object, i + 1) == 0);
                if (sample)
                    lane.samples.push_back(std::chrono::duration<double, std::nano>(
                                               std::chrono::steady_clock::now() - begin)
                                               .count());
            }
            lane.cpu = CpuNs() - cpu;
            done.arrive_and_wait();
        });
    const auto begin = std::chrono::steady_clock::now();
    start.arrive_and_wait();
    done.arrive_and_wait();
    const double elapsed =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
    workers.clear();
    u64 count{}, cpu{};
    std::vector<double> samples;
    for (const auto& lane : lanes) {
        count += lane.count;
        cpu += lane.cpu;
        samples.insert(samples.end(), lane.samples.begin(), lane.samples.end());
    }
    Require(count == threads * iterations);
    std::ranges::sort(samples);
    std::printf("{\"case\":\"pairs\",\"primitive\":\"%s\",\"round\":%u,\"threads\":%u,\"shared\":%"
                "s,\"pairs\":%llu,\"elapsed_ns_per_pair\":%.2f,\"cpu_ns_per_pair\":%.2f,\"sample_"
                "p50_ns\":%.2f,\"sample_p95_ns\":%.2f,\"sample_p99_ns\":%.2f}\n",
                name, round, threads, shared ? "true" : "false", (unsigned long long)count,
                elapsed / count, double(cpu) / count, samples[samples.size() / 2],
                samples[samples.size() * 95 / 100], samples[samples.size() * 99 / 100]);
}
// Measure unrelated wakeups, not inferred lock contention from wall time.
void Isolation(bool assert_isolation) {
    Fixture f;
    GuestSemaphoreDomain sem(*f.space, [&] { return f.Allocate(); });
    GuestRwlockDomain rw(*f.space, [&] { return f.Allocate(); });
    Require(sem.Init(f.base, 0, 0) == 0 && sem.Init(f.base + 8, 0, 0) == 0);
    Require(rw.Init(f.base + 16, 0) == 0 && rw.Init(f.base + 24, 0) == 0);
    Require(rw.Lock(f.base + 16, 99, true, false, {}) == 0);
    std::array<long, 2> switches{};
    std::array<u64, 2> cpu{};
    std::atomic<unsigned> ready{};
    std::stop_source stop;
    auto parked = [&](unsigned i) {
        rusage before{}, after{};
        Require(getrusage(RUSAGE_THREAD, &before) == 0);
        const auto begin = CpuNs();
        ++ready;
        const int result = i == 0 ? sem.Wait(f.base, false, stop.get_token())
                                  : rw.Lock(f.base + 16, 1, false, false, stop.get_token());
        Require(result == POSIX_EINTR);
        cpu[i] = CpuNs() - begin;
        Require(getrusage(RUSAGE_THREAD, &after) == 0);
        switches[i] = after.ru_nvcsw - before.ru_nvcsw;
    };
    std::jthread a([&] { parked(0); }), b([&] { parked(1); });
    while (ready != 2)
        std::this_thread::yield();
    std::this_thread::sleep_for(30ms);
    constexpr unsigned signals = 32;
    for (unsigned i = 0; i < signals; ++i) {
        Require(sem.Post(f.base + 8) == 0 && sem.Wait(f.base + 8, false, {}) == 0);
        Require(rw.Lock(f.base + 24, 2, true, false, {}) == 0 && rw.Unlock(f.base + 24, 2) == 0);
        std::this_thread::sleep_for(3ms);
    }
    stop.request_stop();
    a.join();
    b.join();
    for (unsigned i = 0; i < 2; ++i) {
        std::printf("{\"case\":\"unrelated_wake\",\"primitive\":\"%s\",\"signals\":%u,\"voluntary_"
                    "switches\":%ld,\"thread_cpu_ns\":%llu}\n",
                    i == 0 ? "guest_posix_sem" : "guest_rwlock", signals, switches[i],
                    (unsigned long long)cpu[i]);
        // Allows initial park, cancellation and scheduler noise, not one wake per signal.
        if (assert_isolation)
            Require(switches[i] < signals / 2);
    }
    Require(rw.Unlock(f.base + 16, 99) == 0 && f.space->Counts().live_pins == 0);
}
} // namespace
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const bool isolation_only = argc > 1 && std::string_view(argv[1]) == "--isolation-only";
    const bool verify = argc > 1 && std::string_view(argv[1]) == "--assert-isolation";
    utsname info{};
    Require(uname(&info) == 0);
    std::printf("{\"case\":\"environment\",\"system\":\"%s\",\"release\":\"%s\",\"machine\":\"%s\","
                "\"fex\":false,\"page_size\":%llu}\n",
                info.sysname, info.release, info.machine, (unsigned long long)HostPageSize());
    Isolation(verify);
    if (isolation_only || verify)
        return 0;
    Fixture f;
    GuestMutexDomain mutex(*f.space, [&] { return f.Allocate(); });
    GuestSemaphoreDomain sem(*f.space, [&] { return f.Allocate(); });
    GuestRwlockDomain rw(*f.space, [&] { return f.Allocate(); });
    GuestKernelSemaphore kernel(*f.space);
    std::array<u32, 12> kernel_ids;
    std::array<std::mutex, 12> native;
    std::array<Libraries::Kernel::TimedMutex, 12> desktop;
    std::array<Common::FutexMutex, 12> futex;
    std::array<std::shared_mutex, 12> native_rw;
    std::array<sem_t, 12> native_sem;
    for (unsigned i = 0; i < 12; ++i) {
        Require(mutex.Init(f.base + i * 8, 0) == 0 && sem.Init(f.base + 128 + i * 8, 0, 1) == 0 &&
                rw.Init(f.base + 256 + i * 8, 0) == 0);
        Require(sem_init(&native_sem[i], 0, 1) == 0);
        Require(kernel.Dispatch("188x57JYp0g", {f.base + 384 + i * 4, f.base + 512, 1, 1, 1, 0},
                                700) == 0);
        Require(bool(f.space->ReadData(GuestAddress{f.base + 384 + i * 4},
                                       std::as_writable_bytes(std::span{&kernel_ids[i], 1}))));
    }
    for (unsigned round = 0; round < 3; ++round)
        for (unsigned threads : {1u, 4u, 12u})
            for (bool shared : {false, true}) {
                // Alternate direct/domain ordering to expose thermal/order sensitivity.
                const auto domains = [&] {
                    Throughput(
                        "guest_mutex", round, threads, shared,
                        [&](auto i, auto owner) {
                            return mutex.Lock(f.base + i * 8, owner, false, {});
                        },
                        [&](auto i, auto owner) { return mutex.Unlock(f.base + i * 8, owner); });
                    Throughput(
                        "guest_posix_sem", round, threads, shared,
                        [&](auto i, auto) { return sem.Wait(f.base + 128 + i * 8, false, {}); },
                        [&](auto i, auto) { return sem.Post(f.base + 128 + i * 8); });
                    Throughput(
                        "guest_rwlock_write", round, threads, shared,
                        [&](auto i, auto owner) {
                            return rw.Lock(f.base + 256 + i * 8, owner, true, false, {});
                        },
                        [&](auto i, auto owner) { return rw.Unlock(f.base + 256 + i * 8, owner); });
                    Throughput(
                        "guest_kernel_sema", round, threads, shared,
                        [&](auto i, auto) {
                            return kernel.Dispatch("Zxa0VhQVTsk", {kernel_ids[i], 1, 0}, 700);
                        },
                        [&](auto i, auto) {
                            return kernel.Dispatch("4czppHBiriw", {kernel_ids[i], 1}, 700);
                        });
                };
                const auto natives = [&] {
                    Throughput(
                        "native_mutex", round, threads, shared,
                        [&](auto i, auto) {
                            native[i].lock();
                            return 0;
                        },
                        [&](auto i, auto) {
                            native[i].unlock();
                            return 0;
                        });
                    Throughput(
                        "desktop_timed_mutex", round, threads, shared,
                        [&](auto i, auto) {
                            desktop[i].lock();
                            return 0;
                        },
                        [&](auto i, auto) {
                            desktop[i].unlock();
                            return 0;
                        });
                    Throughput(
                        "native_futex_012", round, threads, shared,
                        [&](auto i, auto) {
                            futex[i].lock();
                            return 0;
                        },
                        [&](auto i, auto) {
                            futex[i].unlock();
                            return 0;
                        });
                    Throughput(
                        "native_posix_sem", round, threads, shared,
                        [&](auto i, auto) { return sem_wait(&native_sem[i]); },
                        [&](auto i, auto) { return sem_post(&native_sem[i]); });
                    Throughput(
                        "native_rwlock_write", round, threads, shared,
                        [&](auto i, auto) {
                            native_rw[i].lock();
                            return 0;
                        },
                        [&](auto i, auto) {
                            native_rw[i].unlock();
                            return 0;
                        });
                };
                if (round % 2) {
                    domains();
                    natives();
                } else {
                    natives();
                    domains();
                }
            }
    for (auto& s : native_sem)
        Require(sem_destroy(&s) == 0);
    Require(f.space->Counts().live_pins == 0);
    std::puts("{\"case\":\"result\",\"status\":\"PASS\"}");
}
