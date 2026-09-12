#define GUEST_CPU_TEST_HOOKS 1
#include "core/guest_cpu/fex/fex_context.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
namespace Core::GuestCpu::Fex {
#define private public
#include "gate-implementation-extracted.inc"
#undef private
}
using namespace std::chrono_literals;
int main() {
    using Core::GuestCpu::Fex::FexTestRunGateImpl;
    {
        FexTestRunGateImpl gate;
        const bool armed = gate.Arm(7);
        const bool wait = gate.WaitAtEntry(11, 7, 1, 1);
        const bool rearm = gate.Arm(7);
        const bool release = gate.Release(1);
        const bool rearm_after_release = gate.Arm(7);
        std::printf("timeout: initial_arm=%d owner_wait=%d rearm=%d release=%d rearm_after_release=%d\n",
                    armed, wait, rearm, release, rearm_after_release);
    }
    bool found = false;
    for (int trial = 0; trial < 100 && !found; ++trial) {
        FexTestRunGateImpl gate;
        gate.Arm(7);
        auto owner = std::async(std::launch::async, [&] { return gate.WaitAtEntry(11, 7, 1, 100); });
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!gate.Arrived() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        if (!gate.Arrived()) return 5;
        // Use the actual Arm()/Release() implementation concurrently. Private data is only READ
        // under its mutex to witness whether the first owner has exited, not modified by the probe.
        auto releaser = std::async(std::launch::async, [&] { return gate.Release(100); });
        while (!gate.Arm(7) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        {
            std::lock_guard lock{gate.mutex_};
            found = gate.armed_ && gate.exited_generation_ == 0;
        }
        const bool second_release = gate.Release(100);
        const bool first_release = releaser.get();
        const bool owner_return = owner.get();
        if (found) {
            std::printf("rearm_race: trial=%d new_arm_accepted_before_prior_exit=1 first_release=%d second_release=%d one_owner_return=%d\n",
                        trial, first_release, second_release, owner_return);
        }
    }
    std::printf("rearm_race_witness=%d\n", found);
    return found ? 0 : 2;
}
