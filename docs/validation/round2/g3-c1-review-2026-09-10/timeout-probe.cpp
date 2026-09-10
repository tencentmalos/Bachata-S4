// Real adapter timeout retirement check. Leaves production and formal tests unchanged.
#define main original_suite_main
#include "guest_execution_tests.cpp"
#undef main
int main() {
#include "../g3-n3-probe-2026-09-10/setup.inc"
    auto& h = harness;
    std::string error;
    if (!LoadFixture(h, *FindFixture("return_only"), error)) return 5;
    auto* gate = static_cast<Fex::FexTestRunGate*>(Fex::FexTestRunGatePointer(*h.context));
    bool reproduced = false;
    {
        TestOwner owner(h, h.data_base);
        auto token = gate->Arm(owner.handle.id, h.context->ContextId());
        auto future = owner.Run();
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!gate->Arrived(token) && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        if (!gate->Arrived(token)) std::_Exit(5);
        auto ticket = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
        if (!ticket) std::_Exit(5);
        if (future.wait_for(std::chrono::seconds(6)) != std::future_status::ready) std::_Exit(5);
        auto result = future.get(); // Owner has fully returned from Run; no scheduling ambiguity.
        auto wait1 = h.context->WaitStopped(ticket.Value(), 100'000'000);
        auto wait2 = h.context->WaitStopped(ticket.Value(), 100'000'000);
        auto lease = h.space->AcquireExecutionLease();
        printf("GATE_TIMEOUT run=%s exited=%d wait1=%s wait2=%s execution_lease=%d\n",
            result ? "OK" : Describe(result.GetError()).c_str(), gate->Exited(token),
            wait1 ? "OK" : Describe(wait1.GetError()).c_str(),
            wait2 ? "OK" : Describe(wait2.GetError()).c_str(), bool(lease));
        reproduced = !result && result.Category() == ErrorCategory::BackendFailure &&
            !wait1 && wait1.Category() == ErrorCategory::Timeout &&
            !wait2 && wait2.Category() == ErrorCategory::Timeout;
    }
    printf("TIMEOUT_REVIEW reproduced=%d owner_destroyed=1\n", reproduced);
    return reproduced ? 2 : 0;
}
