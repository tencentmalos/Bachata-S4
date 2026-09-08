// R3 verification: does a quiescence token block NEW execution from entering, even when every
// thread is already stopped?
//
// Before the fix the token only excluded other writers; Run/CreateThread had no gate, so a Run
// could enter the JIT on either side of the backend's running-thread check.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"
#include "guest_fixtures.h"

using namespace Core::GuestCpu;
constexpr std::uint64_t kCodeOffset = 0x10000, kStackOffset = 0x20000, kMappingSize = 0x4000;

const Fixtures::Fixture* Find(std::string_view n) {
    for (const auto& f : Fixtures::kAll) if (f.name == n) return &f;
    return nullptr;
}

int main() {
    printf("R3 probe: transaction admission with all threads already stopped\n");
    printf("host page size: %ld\n\n", ::sysconf(_SC_PAGESIZE));

    AddressSpaceConfig cfg{};
    cfg.reservation_size = std::uint64_t{1} << 28;
    cfg.max_address = QueryBackendCapabilities().max_guest_address;
    auto sr = GuestAddressSpace::Create(cfg);
    if (!sr) return 2;
    auto space = std::move(sr).Value();

    const std::uint64_t base = space->ReservationBase().value;
    const std::uint64_t code_base = base + kCodeOffset, stack_base = base + kStackOffset;
    const std::uint64_t stack_top = stack_base + kMappingSize - 16;
    const GuestRange code_range{GuestAddress{code_base}, kMappingSize};
    if (!space->Map(code_range, GuestPermission::Read | GuestPermission::Write)) return 2;
    if (!space->Map(GuestRange{GuestAddress{stack_base}, kMappingSize},
                    GuestPermission::Read | GuestPermission::Write)) return 2;

    auto cr = CreateContext(CpuConfig{}, *space);
    if (!cr) return 2;
    auto ctx = std::move(cr).Value();
    const std::uint64_t gate = ctx->Capabilities().return_gate_address;
    const auto* a = Find("constant_a");
    if (!a || gate == 0) return 2;

    // Publish A normally and confirm it runs.
    {
        std::vector<std::byte> img(kMappingSize, std::byte{0});
        std::memcpy(img.data(), a->bytes.data(), a->bytes.size());
        if (a->gate_offset >= 0) std::memcpy(img.data() + a->gate_offset, &gate, sizeof(gate));
        auto q = space->Quiesce(1'000'000'000);
        if (!q) return 2;
        if (!space->PublishCode(q.Value(), code_range, img)) return 2;
    }
    if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Execute)) return 2;

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{code_base};
    init.initial_rsp = GuestAddress{stack_top};
    init.guest_tid = 1;
    auto th = ctx->CreateThread(init);
    if (!th) { printf("FAILED baseline CreateThread\n"); return 2; }
    auto baseline = ctx->Run(th.Value(), RunOptions{});
    printf("baseline run: ok=%d rax=%" PRIu64 "\n\n", int(bool(baseline)),
           baseline ? baseline.Value().snapshot.registers.Get(Gpr::Rax) : 0);

    // Every thread is stopped now. Open a transaction and try to enter execution.
    printf("live threads before transaction = %zu\n", ctx->LiveThreadCount());
    auto q = space->Quiesce(1'000'000'000);
    if (!q) { printf("FAILED quiesce\n"); return 2; }

    auto run_during = ctx->Run(th.Value(), RunOptions{});
    printf("Run during transaction      = %d", int(bool(run_during)));
    if (!run_during) printf("  [%s]", Describe(run_during.GetError()).c_str());
    else printf("  rax=%" PRIu64, run_during.Value().snapshot.registers.Get(Gpr::Rax));
    printf("\n");

    auto create_during = ctx->CreateThread(init);
    printf("CreateThread during transaction = %d", int(bool(create_during)));
    if (!create_during) printf("  [%s]", Describe(create_during.GetError()).c_str());
    printf("\n");
    if (create_during) (void)ctx->DestroyThread(create_during.Value());

    // Release the transaction; execution must work again.
    { auto released = std::move(q).Value(); (void)released; }
    auto run_after = ctx->Run(th.Value(), RunOptions{});
    printf("Run after transaction       = %d", int(bool(run_after)));
    if (run_after) printf("  rax=%" PRIu64, run_after.Value().snapshot.registers.Get(Gpr::Rax));
    else printf("  [%s]", Describe(run_after.GetError()).c_str());
    printf("\n");
    (void)ctx->DestroyThread(th.Value());

    printf("\n--- observations ---\n");
    printf("R3_run_admitted_during_transaction=%d\n", int(bool(run_during)));
    printf("R3_create_admitted_during_transaction=%d\n", int(bool(create_during)));
    printf("R3_execution_restored_after_release=%d\n", int(bool(run_after)));
    return 0;
}
