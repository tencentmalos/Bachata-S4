// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <fmt/format.h>

#include "core/libraries/fiber/fiber.h"
#include "core/libraries/kernel/threads/pthread.h"

#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "ntapi.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // CPU time to grant.
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    // Determine the timebase for converting time to ticks.
    struct mach_timebase_info timebase{};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        // Should not matter since preemptible is false, but needs to be >= computation regardless.
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // Not implemented
}

#endif

#ifdef _WIN32

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();

    LARGE_INTEGER interval{
        .QuadPart = -1 * (duration.count() / 100u),
    };
    // One timer per thread instead of creating and closing one per sleep. High resolution timers
    // (Windows 10 1803+) expire close to the request instead of on the next scheduler tick.
    struct ThreadTimer {
        HANDLE handle;
        ThreadTimer() {
            handle = ::CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
            if (handle == nullptr) {
                handle = ::CreateWaitableTimerW(nullptr, TRUE, nullptr);
            }
        }
        ~ThreadTimer() {
            if (handle != nullptr) {
                ::CloseHandle(handle);
            }
        }
    };
    thread_local ThreadTimer thread_timer;
    thread_local u32 sleep_depth = 0;
    // A guest signal delivered as an APC during this wait may sleep too: it must not re-arm the
    // timer the interrupted sleep is waiting on.
    std::optional<ThreadTimer> nested_timer;
    HANDLE timer = thread_timer.handle;
    if (sleep_depth != 0) {
        timer = nested_timer.emplace().handle;
    }
    ++sleep_depth;
    SetWaitableTimer(timer, &interval, 0, NULL, NULL, 0);
    const auto ret = WaitForSingleObjectEx(timer, INFINITE, interruptible);
    --sleep_depth;

    if (remaining) {
        const auto end_sleep = std::chrono::high_resolution_clock::now();
        const auto sleep_time = end_sleep - begin_sleep;
        *remaining = duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

#else

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain;
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0 || errno != EINTR;
}

#endif

#ifdef _WIN32

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else // !_WIN32, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
    // TODO
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    // Do Nothing on MinGW
}

void SetThreadName(void* thread, const char* name) {
    // Do Nothing on MinGW
}
#endif

#endif

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        if (g_curthread->tcb->tcb_fiber) {
            return fmt::format("{}@@{}", g_curthread->name,
                               g_curthread->tcb->tcb_fiber->current_fiber->name);
        }
        return g_curthread->name;
    }
#ifdef _WIN32
    PWSTR name{};
    if (FAILED(GetThreadDescription(GetCurrentThread(), &name)) || name == nullptr) {
        return "<unknown name>";
    }
    const auto result = Common::UTF16ToUTF8(name);
    LocalFree(name);
    return result;
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

// ---- Host thread priorities -------------------------------------------------------------------

bool HostThreadPrioritiesEnabled() {
    static const bool enabled = [] {
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.shadps4.thread_priority", value);
        return std::string_view(value) != "0";
#else
        const char* value = std::getenv("SHADPS4_THREAD_PRIORITY");
        return !(value && std::string_view(value) == "0");
#endif
    }();
    return enabled;
}

int ThreadPriorityNice(ThreadPriority priority) {
    switch (priority) {
    case ThreadPriority::Low:
        return 4;
    case ThreadPriority::Normal:
        return 0;
    case ThreadPriority::High:
        return -4;
    case ThreadPriority::VeryHigh:
        return -6;
    case ThreadPriority::Critical:
        return -8;
    }
    return 0;
}

NativeThreadRef CurrentNativeThreadRef() {
#ifdef _WIN32
    return static_cast<NativeThreadRef>(GetCurrentThreadId());
#elif defined(__APPLE__)
    return reinterpret_cast<NativeThreadRef>(pthread_self());
#elif defined(__ANDROID__)
    return static_cast<NativeThreadRef>(gettid());
#elif defined(__linux__)
    return static_cast<NativeThreadRef>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
// Lowest nice an unprivileged thread may set: 20 - RLIMIT_NICE (Android init raises the limit to
// 40, so -20; a desktop Linux default of 0 gives 20, i.e. nice can only go up).
int NiceFloor() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NICE, &limit) != 0) {
        return 0;
    }
    const rlim_t current =
        limit.rlim_cur == RLIM_INFINITY ? 40 : std::min<rlim_t>(limit.rlim_cur, 40);
    return 20 - static_cast<int>(current);
}
#endif

} // namespace

int ThreadNiceBase() {
#if !defined(_WIN32) && !defined(__APPLE__)
    static const int base = [] {
        errno = 0;
        const int value = getpriority(PRIO_PROCESS, static_cast<id_t>(CurrentNativeThreadRef()));
        return errno == 0 ? value : 0;
    }();
    return base;
#else
    return 0;
#endif
}

std::optional<int> SetThreadNice(NativeThreadRef thread, int offset) {
    if (!HostThreadPrioritiesEnabled() || thread == 0) {
        return std::nullopt;
    }
    int nice = std::clamp(offset, -20, 19);
#ifdef _WIN32
    int level = THREAD_PRIORITY_NORMAL;
    if (nice <= -6) {
        level = THREAD_PRIORITY_HIGHEST;
    } else if (nice <= -3) {
        level = THREAD_PRIORITY_ABOVE_NORMAL;
    } else if (nice >= 10) {
        level = THREAD_PRIORITY_LOWEST;
    } else if (nice >= 3) {
        level = THREAD_PRIORITY_BELOW_NORMAL;
    }
    const HANDLE handle =
        OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread));
    if (handle == nullptr) {
        return std::nullopt;
    }
    const bool ok = SetThreadPriority(handle, level) != 0;
    CloseHandle(handle);
    return ok ? std::optional<int>(nice) : std::nullopt;
#elif defined(__APPLE__)
    // SCHED_OTHER priorities 15..47, default 31; a higher number runs first.
    sched_param param{};
    param.sched_priority = std::clamp(31 - nice, 15, 47);
    if (pthread_setschedparam(reinterpret_cast<pthread_t>(thread), SCHED_OTHER, &param) != 0) {
        return std::nullopt;
    }
    return nice;
#else
    const int base = ThreadNiceBase();
    const int floor = NiceFloor();
    if (floor > base) {
        // A thread moved above the base could never be brought back (e.g. a guest thread that
        // restores its priority): leave scheduling alone rather than make it one-way.
        return std::nullopt;
    }
    const int target = std::clamp(base + nice, std::max(floor, -20), 19);
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(thread), target) != 0) {
        return std::nullopt;
    }
    return target;
#endif
}

std::optional<int> SetCurrentThreadNice(int offset) {
    return SetThreadNice(CurrentNativeThreadRef(), offset);
}

const CpuCapacitySplit& GetCpuCapacitySplit() {
    static const CpuCapacitySplit split = [] {
        CpuCapacitySplit result;
#if defined(__linux__)
        const long count = sysconf(_SC_NPROCESSORS_CONF);
        if (count <= 1 || count > CPU_SETSIZE) {
            result.note = count <= 1 ? "one cpu" : "too many cpus";
            return result;
        }
        std::vector<long> capacity;
        for (long cpu = 0; cpu < count; ++cpu) {
            long value = -1;
            const auto path = fmt::format("/sys/devices/system/cpu/cpu{}/cpu_capacity", cpu);
            if (FILE* file = std::fopen(path.c_str(), "r")) {
                if (std::fscanf(file, "%ld", &value) != 1) {
                    value = -1;
                }
                std::fclose(file);
            }
            if (value <= 0) {
                result.note = "no cpu_capacity";
                return result;
            }
            capacity.push_back(value);
        }
        const long lowest = *std::min_element(capacity.begin(), capacity.end());
        for (long cpu = 0; cpu < count; ++cpu) {
            (capacity[cpu] == lowest ? result.lowest : result.others)
                .push_back(static_cast<int>(cpu));
        }
        if (result.others.empty()) {
            result.lowest.clear();
            result.note = "symmetric";
        }
#else
        result.note = "unsupported platform";
#endif
        return result;
    }();
    return split;
}

bool SetThreadAffinity(NativeThreadRef thread, const std::vector<int>& cpus) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cpus.empty()) {
        const long count = sysconf(_SC_NPROCESSORS_CONF);
        for (long cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
            CPU_SET(cpu, &set);
        }
    } else {
        for (const int cpu : cpus) {
            if (cpu >= 0 && cpu < CPU_SETSIZE) {
                CPU_SET(cpu, &set);
            }
        }
    }
    return sched_setaffinity(static_cast<pid_t>(thread), sizeof(set), &set) == 0;
#else
    (void)thread;
    (void)cpus;
    return false;
#endif
}

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    static constexpr const char* Names[] = {"host Low", "host Normal", "host High", "host VeryHigh",
                                            "host Critical"};
    const int nice = ThreadPriorityNice(new_priority);
    const auto applied = SetCurrentThreadNice(nice);
    const auto index = static_cast<u32>(new_priority);
    NoteThreadPriority(CurrentNativeThreadRef(), GetCurrentThreadName(),
                       index < std::size(Names) ? Names[index] : "host", nice, applied);
}

namespace {

struct PriorityRecord {
    std::string name;
    std::string source;
    int requested{};
    std::optional<int> applied;
    u64 changes{};
};

struct PriorityRegistry {
    std::mutex mutex;
    std::map<NativeThreadRef, PriorityRecord> threads;
    std::atomic<u32> logged{};
};

PriorityRegistry& Registry() {
    // Never destroyed: threads may still report while statics are torn down.
    static auto* registry = new PriorityRegistry;
    return *registry;
}

} // namespace

void NoteThreadPriority(NativeThreadRef thread, std::string_view name, std::string_view source,
                        int requested, std::optional<int> applied) {
    auto& registry = Registry();
    if (registry.logged.fetch_add(1, std::memory_order_relaxed) < 128) {
        if (applied) {
            LOG_INFO(Common, "Thread priority: {} ({}) offset {} -> host {}", name, source,
                     requested, *applied);
        } else {
            LOG_INFO(Common, "Thread priority: {} ({}) offset {} not applied{}", name, source,
                     requested,
                     HostThreadPrioritiesEnabled() ? "" : " (thread priorities disabled)");
        }
    }
    if (thread == 0) {
        return;
    }
    std::scoped_lock lock{registry.mutex};
    if (registry.threads.size() >= 1024 && !registry.threads.contains(thread)) {
        return;
    }
    auto& record = registry.threads[thread];
    record.name = name;
    record.source = source;
    record.requested = requested;
    record.applied = applied;
    ++record.changes;
}

std::string ThreadPriorityCommand(const std::vector<std::string>& args) {
    auto& registry = Registry();
    if (!args.empty() && args[0] == "reset") {
        std::scoped_lock lock{registry.mutex};
        registry.threads.clear();
        return "thread_priority: reset\n";
    }
    if (!args.empty() && args[0] != "status") {
        return "usage: thread_priority status | reset\n";
    }
    std::string out = fmt::format("thread_priority: enabled={}", HostThreadPrioritiesEnabled());
#if !defined(_WIN32) && !defined(__APPLE__)
    out += fmt::format(" nice_base={} nice_floor={}", ThreadNiceBase(), NiceFloor());
#endif
    out += "\n";
    std::scoped_lock lock{registry.mutex};
    for (const auto& [thread, record] : registry.threads) {
        std::string current = "?";
#if !defined(_WIN32) && !defined(__APPLE__)
        errno = 0;
        const int value = getpriority(PRIO_PROCESS, static_cast<id_t>(thread));
        current = errno == 0 ? std::to_string(value) : "gone";
#endif
        out += fmt::format("  {} {:<24} {:<28} offset={} applied={} now={} changes={}\n", thread,
                           record.name, record.source, record.requested,
                           record.applied ? std::to_string(*record.applied) : "none", current,
                           record.changes);
    }
    return out;
}

} // namespace Common
