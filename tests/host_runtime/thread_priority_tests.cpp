// SPDX-License-Identifier: GPL-2.0-or-later
// Host thread priorities (common/thread.h): nice offsets applied to the current thread and to
// another thread of the process, relative to the nice the process started with (an Android
// foreground app starts its threads at -10), the RLIMIT_NICE floor, the emulator thread classes,
// the guest-priority mapping and the thread_priority DebugBus text.
// Run it at different starting nice values, e.g. `nice -n -10 ./thread_priority_tests`.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/libraries/kernel/threads/host_priority.h"

namespace {
unsigned checks{}, failures{};
void Check(const std::string& name, bool ok) {
    ++checks;
    failures += !ok;
    std::printf("[TP%02u] %s %s\n", checks, ok ? "PASS" : "FAIL", name.c_str());
    std::fflush(stdout);
}
int CurrentNice(Common::NativeThreadRef thread) {
    errno = 0;
    return getpriority(PRIO_PROCESS, static_cast<id_t>(thread));
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = std::filesystem::current_path() / "thread-priority-root";
    std::filesystem::create_directories(root);
    Common::FS::InitializeAndroidUserPaths(root);
    Common::Log::Setup("thread-priority-tests");

    using Libraries::Kernel::GuestPriorityToHostNice;
    Check("mapping FIFO 256 -> -4", GuestPriorityToHostNice(1, 256) == -4);
    Check("mapping FIFO 500 -> -2", GuestPriorityToHostNice(1, 500) == -2);
    Check("mapping FIFO default 700 -> 0", GuestPriorityToHostNice(1, 700) == 0);
    Check("mapping RR 767 -> 2", GuestPriorityToHostNice(3, 767) == 2);
    Check("mapping OTHER 900 -> 4", GuestPriorityToHostNice(2, 900) == 4);
    Check("mapping out of range -> 0", GuestPriorityToHostNice(1, 42) == 0);

    const auto self = Common::CurrentNativeThreadRef();
    // Read before any change: this is what the process's threads inherited.
    const int start = CurrentNice(self);
    const bool enabled = Common::HostThreadPrioritiesEnabled();
    rlimit limit{};
    getrlimit(RLIMIT_NICE, &limit);
    const int floor = 20 - static_cast<int>(limit.rlim_cur == RLIM_INFINITY
                                                ? 40
                                                : std::min<rlim_t>(limit.rlim_cur, 40));
    const auto expect = [&](int offset) {
        return std::clamp(start + std::clamp(offset, -20, 19), std::max(floor, -20), 19);
    };
    std::printf("enabled=%d start_nice=%d RLIMIT_NICE=%llu floor=%d uid=%d\n", enabled, start,
                static_cast<unsigned long long>(limit.rlim_cur), floor, getuid());
    Check("current thread ref is the kernel tid",
          self == static_cast<Common::NativeThreadRef>(gettid()));

    if (!enabled) {
        Check("disabled: no change",
              !Common::SetCurrentThreadNice(-4).has_value() && CurrentNice(self) == start);
    } else if (floor > start) {
        // A thread moved above the base could not be brought back: refuse rather than go one-way.
        Check("floor above base: change refused",
              !Common::SetCurrentThreadNice(4).has_value() && CurrentNice(self) == start);
    } else {
        auto applied = Common::SetCurrentThreadNice(-4);
        Check("base is the starting nice", Common::ThreadNiceBase() == start);
        Check("offset -4 applied on the base",
              applied == expect(-4) && CurrentNice(self) == *applied);
        applied = Common::SetCurrentThreadNice(4);
        Check("offset +4 applied on the base",
              applied == expect(4) && CurrentNice(self) == *applied);
        applied = Common::SetCurrentThreadNice(0);
        Check("offset 0 is the base again", applied == start && CurrentNice(self) == start);
        applied = Common::SetCurrentThreadNice(-30);
        Check("clamped to the floor", applied == expect(-30) && CurrentNice(self) == *applied);
        Common::SetCurrentThreadNice(0);

        // Another thread, changed from here (the guest setprio path).
        std::mutex mutex;
        std::condition_variable cv;
        Common::NativeThreadRef worker_ref{};
        bool done{};
        int seen{};
        std::thread worker([&] {
            std::unique_lock lock{mutex};
            worker_ref = Common::CurrentNativeThreadRef();
            cv.notify_all();
            cv.wait(lock, [&] { return done; });
            seen = CurrentNice(Common::CurrentNativeThreadRef());
        });
        {
            std::unique_lock lock{mutex};
            cv.wait(lock, [&] { return worker_ref != 0; });
        }
        applied = Common::SetThreadNice(worker_ref, -2);
        Check("other thread -2 applied",
              applied == expect(-2) && CurrentNice(worker_ref) == *applied);
        Check("main thread unchanged", CurrentNice(self) == start);
        {
            std::scoped_lock lock{mutex};
            done = true;
        }
        cv.notify_all();
        worker.join();
        Check("worker saw its own nice", seen == *applied);

        // A thread created by a moved thread inherits the moved value; offset 0 still means the
        // process base, not the creator's value.
        int child_start{}, child_after{};
        std::thread parent([&] {
            Common::SetCurrentThreadNice(-4);
            std::thread child([&] {
                child_start = CurrentNice(Common::CurrentNativeThreadRef());
                Common::SetCurrentThreadNice(0);
                child_after = CurrentNice(Common::CurrentNativeThreadRef());
            });
            child.join();
        });
        parent.join();
        Check("child inherits the creator's nice", child_start == expect(-4));
        Check("child offset 0 returns to the process base", child_after == start);

        // Emulator thread classes.
        std::thread high([&] {
            Common::SetCurrentThreadName("tp:High");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
            seen = CurrentNice(Common::CurrentNativeThreadRef());
        });
        high.join();
        Check("ThreadPriority::High -> base-4", seen == expect(-4));
        std::thread low([&] {
            Common::SetCurrentThreadName("tp:Low");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            seen = CurrentNice(Common::CurrentNativeThreadRef());
        });
        low.join();
        Check("ThreadPriority::Low -> base+4", seen == expect(4));
    }
    Check("SetThreadNice(0 ref) does nothing", !Common::SetThreadNice(0, -4).has_value());

    const auto status = Common::ThreadPriorityCommand({"status"});
    std::printf("%s", status.c_str());
    Check("status reports the base",
          status.find("nice_base=" + std::to_string(Common::ThreadNiceBase())) !=
              std::string::npos);
    Check("status lists the emulator threads", !enabled || floor > start ||
                                                   (status.find("tp:High") != std::string::npos &&
                                                    status.find("host High") != std::string::npos &&
                                                    status.find("tp:Low") != std::string::npos));
    Check("reset clears",
          Common::ThreadPriorityCommand({"reset"}).find("reset") != std::string::npos &&
              Common::ThreadPriorityCommand({}).find("tp:High") == std::string::npos);

    std::printf("thread priority: %u checks, %u failures\n", checks, failures);
    Common::Log::Shutdown();
    return failures ? 1 : 0;
}
