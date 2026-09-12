// Review the real Run-entry timeout with and without a controller's pending Pause.
#define main original_suite_main
#include "guest_execution_tests.cpp"
#undef main
int main(int argc, char** argv) {
#include "../g3-n3-probe-2026-09-10/setup.inc"
    auto& h = harness;
    if (!LoadProgress(h)) return 5;
    bool pending = argc > 1 && std::string(argv[1]) == "pending";
    auto progress = PrepareProgress(h);
    TestOwner owner(h, progress);
    auto* gate = static_cast<Fex::FexTestRunGate*>(Fex::FexTestRunGatePointer(*h.context));
    auto token = gate->Arm(owner.handle.id, h.context->ContextId());
    auto future = owner.Run();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gate->Arrived(token) && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    if (!gate->Arrived(token)) std::_Exit(5);
    std::optional<InterruptTicket> pause;
    if (pending) pause = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause).Value();
    bool ready = future.wait_for(std::chrono::milliseconds(6500)) == std::future_status::ready;
    auto advanced = Progress(progress);
    printf("TIMEOUT pending=%d gate_exited=%d run_returned=%d progress=%llu\n",
           pending, gate->Exited(token), ready, (unsigned long long)advanced);
    auto ticket = pause ? *pause : h.context->RequestInterrupt(owner.handle, InterruptReason::Cancel).Value();
    auto receipt = h.context->WaitStopped(ticket, 1'000'000'000);
    auto result = Await(future);
    printf("CLEANUP external_cancel=%d receipt=%d result=%s\n", !pending, bool(receipt),
           result ? std::string(ToString(result.Value().primary_reason)).c_str() : "error");
    // A gate expiry is not permission to run an unbounded guest without an explicit release.
    return ready && advanced == 0 && receipt ? 0 : 2;
}
