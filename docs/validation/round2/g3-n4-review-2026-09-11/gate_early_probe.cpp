#include "core/guest_cpu/fex/test_run_gate.h"
#include <cstdio>
int main() {
    Core::GuestCpu::TestGate::RunGate gate;
    auto first = gate.Arm(11, 7);
    bool release_early = gate.Release(first, 10);
    bool owner_clean = gate.WaitAtEntry(7, 11, 1, 10);
    bool repeat_before = gate.Release(first, 10);
    auto second = gate.Arm(12, 7);
    bool repeat_after = gate.Release(first, 10);
    printf("EARLY first=%llu second=%llu release_early=%d owner_clean=%d repeat_before=%d "
           "repeat_after=%d\n", (unsigned long long)first, (unsigned long long)second,
           release_early, owner_clean, repeat_before, repeat_after);
    return !owner_clean && repeat_before == repeat_after ? 0 : 2;
}
