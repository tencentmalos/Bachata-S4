// Review-only bounded public-API counterexample; uses the exact shared gate implementation.
#include "core/guest_cpu/fex/test_run_gate.h"
#include <atomic>
#include <cstdio>
#include <thread>
using Core::GuestCpu::TestGate::RunGate;
using namespace std::chrono_literals;
template<class Predicate> bool Until(Predicate p) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!p()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::yield();
    }
    return true;
}
int main() {
    for (int iteration = 1; iteration <= 100; ++iteration) {
        RunGate gate;
        auto t1 = gate.Arm(11, 7);
        std::atomic<std::uint64_t> t2{0};
        std::atomic<bool> owner1_clean{false};
        std::thread owner1([&] {
            owner1_clean = gate.WaitAtEntry(7, 11, 3, 2000);
            t2 = gate.Arm(12, 7); // Legal after the old owner's exit, before Release returns.
        });
        if (!Until([&] { return gate.Arrived(t1); })) { owner1.join(); return 5; }
        std::atomic<bool> r1_done{false}, r1_result{false};
        std::thread release1([&] {
            r1_result = gate.Release(t1, 100);
            r1_done = true;
        });
        if (!Until([&] { return t2.load() != 0; })) {
            release1.join(); owner1.join(); return 5;
        }
        bool owner2_clean = true;
        auto token2 = t2.load();
        std::thread owner2([&] { owner2_clean = gate.WaitAtEntry(7, 12, 4, 500); });
        if (!Until([&] { return gate.Arrived(token2); })) {
            release1.join(); owner1.join(); owner2.join(); return 5;
        }
        release1.join();
        bool r2_result = gate.Release(token2, 1000);
        owner1.join(); owner2.join();
        if (!r1_result || !r2_result || !owner2_clean) {
            printf("REPRO iteration=%d t1=%llu t2=%llu owner1_clean=%d "
                "release1=%d release2=%d owner2_clean=%d\n", iteration,
                (unsigned long long)t1, (unsigned long long)token2,
                owner1_clean.load(), r1_result.load(), r2_result, owner2_clean);
            return 2;
        }
    }
    printf("NO_REPRO within 100 attempts\n");
    return 0;
}
