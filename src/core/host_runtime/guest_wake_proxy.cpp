// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/guest_wake_proxy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <fmt/format.h>
#include <fmt/ranges.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "common/spin_lock.h"
#include "common/thread.h"
#include "common/types.h"

namespace Core::HostRuntime::WakeProxy {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto Relaxed = std::memory_order_relaxed;
constexpr auto SeqCst = std::memory_order_seq_cst;

// Posts beyond this are notified by the poster itself: on a little core the proxy issues a wake
// in about 55 us, so a deeper queue would delay the woken threads more than the wake costs the
// poster.
constexpr size_t Capacity = 32;

struct Entry {
    NotifyFn notify;
    std::shared_ptr<void> waiter;
};

void Futex(std::atomic<u32>* word, int op, u32 value) {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<u32*>(word), op | FUTEX_PRIVATE_FLAG, value, nullptr,
            nullptr, 0);
#else
    (void)word;
    (void)op;
    (void)value;
#endif
}

// Returns once `word` may differ from `seen`: on arm64 the core waits for an event (a store to
// the word's cache line clears the exclusive monitor; the timer event stream bounds the wait).
void WaitForChange(const std::atomic<u32>& word, u32 seen) {
#if defined(__aarch64__)
    u32 value;
    asm volatile("sevl\n"
                 "wfe\n"
                 "ldxr %w[value], %[word]\n"
                 "eor %w[value], %w[value], %w[seen]\n"
                 "cbnz %w[value], 1f\n"
                 "wfe\n"
                 "1:"
                 : [value] "=&r"(value)
                 : [word] "Q"(*reinterpret_cast<const volatile u32*>(&word)), [seen] "r"(seen)
                 : "memory");
#else
    (void)word;
    (void)seen;
    std::this_thread::yield();
#endif
}

// The proxy should not take a fast core from the game: the lowest-capacity CPUs, or none when
// the topology is unknown or symmetric.
struct Placement {
    std::vector<int> cpus;
    std::string description{"unpinned"};
};

Placement LowestCapacityCpus() {
    Placement placement;
    const auto& split = Common::GetCpuCapacitySplit();
    if (split.lowest.empty()) {
        placement.description = "unpinned (" + split.note + ")";
        return placement;
    }
    placement.cpus = split.lowest;
    placement.description = "cpus " + fmt::format("{}", fmt::join(split.lowest, ","));
    return placement;
}

class Proxy {
public:
    Proxy() : placement{LowestCapacityCpus()} {
        queue.reserve(Capacity);
        std::thread([this] { Run(); }).detach();
    }

    bool Post(NotifyFn notify, std::shared_ptr<void>& waiter) {
        {
            std::scoped_lock lock{queue_lock};
            if (queue.size() >= Capacity) {
                full.fetch_add(1, Relaxed);
                return false;
            }
            queue.push_back({notify, std::move(waiter)});
            if (queue.size() > max_depth.load(Relaxed)) {
                max_depth.store(queue.size(), Relaxed);
            }
        }
        posts.fetch_add(1, Relaxed);
        // Pairs with the sleep check in Run: either the proxy sees this post, or this sees the
        // proxy asleep and wakes it.
        posted.fetch_add(1, SeqCst);
        if (awake.load(SeqCst) == 0 && awake.exchange(1, SeqCst) == 0) {
            kicks.fetch_add(1, Relaxed);
            Futex(&awake, FUTEX_WAKE, 1);
        }
        return true;
    }

    std::string Status() const {
        return fmt::format("placement: {}{}\nspin_us: {}\nposts: {}\nkicks: {}\nfull: {}\n"
                           "notifies: {}\nbatches: {}\nsleeps: {}\nspin_ms: {}\nmax_depth: {}\n",
                           placement.description, pin_failed.load(Relaxed) ? " (failed)" : "",
                           spin_us.load(Relaxed), posts.load(Relaxed),
                           kicks.load(Relaxed), full.load(Relaxed), notifies.load(Relaxed),
                           batches.load(Relaxed), sleeps.load(Relaxed),
                           spin_ns.load(Relaxed) / 1000000, max_depth.load(Relaxed));
    }

    std::atomic<u32> spin_us{200};

private:
    void Run() {
        Common::SetCurrentThreadName("shadPS4:Waker");
        if (!placement.cpus.empty() &&
            !Common::SetThreadAffinity(Common::CurrentNativeThreadRef(), placement.cpus)) {
            pin_failed.store(true, Relaxed);
        }
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        std::vector<Entry> batch;
        batch.reserve(Capacity);
        for (;;) {
            const u32 seen = posted.load(std::memory_order_acquire);
            {
                std::scoped_lock lock{queue_lock};
                batch.swap(queue);
            }
            if (!batch.empty()) {
                for (auto& entry : batch) {
                    entry.notify(entry.waiter.get());
                }
                notifies.fetch_add(batch.size(), Relaxed);
                batches.fetch_add(1, Relaxed);
                batch.clear();
                continue;
            }
            // Stay awake for the spin window: posts come in bursts (a job system waking its
            // workers one after another).
            const auto start = Clock::now();
            const auto window = std::chrono::microseconds(spin_us.load(Relaxed));
            bool again = false;
            for (;;) {
                if (posted.load(std::memory_order_acquire) != seen) {
                    again = true;
                    break;
                }
                if (Clock::now() - start >= window) {
                    break;
                }
                WaitForChange(posted, seen);
            }
            spin_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(),
                Relaxed);
            if (again) {
                continue;
            }
            awake.store(0, SeqCst);
            if (posted.load(SeqCst) != seen) {
                awake.store(1, Relaxed);
                continue;
            }
            sleeps.fetch_add(1, Relaxed);
            while (awake.load(std::memory_order_acquire) == 0) {
                Futex(&awake, FUTEX_WAIT, 0);
            }
        }
    }

    // Each alone in its cache line: `posted` is what the idle proxy watches.
    alignas(64) std::atomic<u32> posted{0};
    alignas(64) std::atomic<u32> awake{1};
    alignas(64) Common::SpinLock queue_lock;
    std::vector<Entry> queue;
    const Placement placement;
    std::atomic<bool> pin_failed{};
    std::atomic<u64> posts{}, kicks{}, full{}, notifies{}, batches{}, sleeps{}, spin_ns{};
    std::atomic<u64> max_depth{};
};

std::mutex instance_mutex;
Proxy* instance{};

// Created on first use and never destroyed: posts can race with process teardown. Null when
// the thread cannot be started; posters then notify themselves.
Proxy* Instance() {
    std::scoped_lock lock{instance_mutex};
    if (!instance) {
        try {
            instance = new Proxy();
        } catch (const std::exception&) {
            return nullptr;
        }
    }
    return instance;
}

bool PostToInstance(NotifyFn notify, std::shared_ptr<void>& waiter) {
    return instance->Post(notify, waiter);
}

} // namespace

void Enable(bool on) {
    post.store(on && Instance() != nullptr ? &PostToInstance : nullptr,
               std::memory_order_release);
}

std::string Command(const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "status" : args[0];
    if (sub == "on" && args.size() == 1) {
        Enable(true);
    } else if (sub == "off" && args.size() == 1) {
        Enable(false);
    } else if (sub == "spin" && args.size() == 2) {
        char* end = nullptr;
        const long us = std::strtol(args[1].c_str(), &end, 10);
        if (end == args[1].c_str() || *end != '\0' || us < 0 || us > 100000) {
            return "status: invalid_arguments\n";
        }
        if (Proxy* proxy = Instance()) {
            proxy->spin_us.store(static_cast<u32>(us));
        }
    } else if (sub != "status" || args.size() != 1) {
        return "status: invalid_arguments\n";
    }
    std::string out = fmt::format("wake_proxy: {}\n", post.load() != nullptr ? "on" : "off");
    std::scoped_lock lock{instance_mutex};
    if (instance) {
        out += instance->Status();
    }
    return out;
}

} // namespace Core::HostRuntime::WakeProxy
