// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/guest_cpu_placement.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <fmt/ranges.h>
#if defined(__linux__)
#include <dirent.h>
#endif

#include "common/thread.h"
#include "common/types.h"

namespace Core::HostRuntime::GuestPlacement {
namespace {

constexpr auto Relaxed = std::memory_order_relaxed;
constexpr size_t MinFasterCpus = 4;

std::atomic<bool> enabled{false};
std::atomic<u64> applied{0};
std::atomic<u64> failed{0};

const std::vector<int>& TargetCpus() {
    static const std::vector<int> cpus = [] {
        const auto& others = Common::GetCpuCapacitySplit().others;
        return others.size() >= MinFasterCpus ? others : std::vector<int>{};
    }();
    return cpus;
}

// The live threads named Guest-N, read from /proc rather than kept in a registry whose thread
// ids could be reused once a thread exits.
std::vector<int> LiveGuestThreads() {
    std::vector<int> tids;
#if defined(__linux__)
    DIR* dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return tids;
    }
    while (const dirent* entry = readdir(dir)) {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0) {
            continue;
        }
        const auto path = fmt::format("/proc/self/task/{}/comm", tid);
        if (FILE* file = std::fopen(path.c_str(), "r")) {
            char comm[32]{};
            if (std::fgets(comm, sizeof(comm), file) != nullptr &&
                std::strncmp(comm, "Guest-", 6) == 0) {
                tids.push_back(tid);
            }
            std::fclose(file);
        }
    }
    closedir(dir);
#endif
    return tids;
}

void Note(bool ok) {
    (ok ? applied : failed).fetch_add(1, Relaxed);
}

} // namespace

void Enable(bool on) {
    enabled.store(on, Relaxed);
    if (TargetCpus().empty()) {
        return;
    }
    // An empty list lets a thread run on every CPU again.
    static const std::vector<int> unrestricted;
    for (const int tid : LiveGuestThreads()) {
        Note(Common::SetThreadAffinity(static_cast<Common::NativeThreadRef>(tid),
                                       on ? TargetCpus() : unrestricted));
    }
}

void ApplyToCurrentThread() {
    if (!enabled.load(Relaxed) || TargetCpus().empty()) {
        return;
    }
    Note(Common::SetThreadAffinity(Common::CurrentNativeThreadRef(), TargetCpus()));
}

std::string Command(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        Enable(args[0] == "on");
    } else if (!args.empty() && args[0] != "status") {
        return "usage: guest_affinity on | off | status\n";
    }
    const auto& split = Common::GetCpuCapacitySplit();
    const std::string target =
        !TargetCpus().empty()
            ? fmt::format("{}", fmt::join(TargetCpus(), ","))
            : fmt::format("none ({})", split.others.empty() ? split.note
                                                             : "fewer than four faster cpus");
    return fmt::format("guest_affinity: {}\ncpus: {}\nlowest: {}\napplied: {}\nfailed: {}\n",
                       enabled.load(Relaxed) ? "on" : "off", target,
                       fmt::join(split.lowest, ","), applied.load(Relaxed),
                       failed.load(Relaxed));
}

} // namespace Core::HostRuntime::GuestPlacement
