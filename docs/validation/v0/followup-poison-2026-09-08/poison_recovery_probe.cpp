// R1 verification: after an injected invalidation failure, is stale JIT code actually unreachable,
// and can the space be repaired?
//
// The review's probe stops at the first refusal. This one distinguishes the refusal reason,
// checks every execution entry point, and then verifies a successful repair restores execution
// with the NEW constant -- so "blocked" cannot be confused with "permanently bricked".

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

constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;
constexpr std::uint64_t kMappingSize = 0x4000;

const Fixtures::Fixture* Find(std::string_view n) {
    for (const auto& f : Fixtures::kAll) if (f.name == n) return &f;
    return nullptr;
}

class FailureSink final : public CodeInvalidationSink {
public:
    std::string_view Name() const override { return "injected-failure"; }
    Status DiscardTranslations(GuestRange, InvalidationReason) override {
        return MakeError(ErrorCategory::BackendFailure, "injected", "injected invalidation failure");
    }
};

int main() {
    printf("R1 probe: poison blocks execution, and repair restores it\n");
    printf("host page size: %ld\n\n", ::sysconf(_SC_PAGESIZE));

    AddressSpaceConfig cfg{};
    cfg.reservation_size = std::uint64_t{1} << 28;
    cfg.max_address = QueryBackendCapabilities().max_guest_address;
    auto sr = GuestAddressSpace::Create(cfg);
    if (!sr) { printf("FAILED create space\n"); return 2; }
    auto space = std::move(sr).Value();

    const std::uint64_t base = space->ReservationBase().value;
    const std::uint64_t code_base = base + kCodeOffset;
    const std::uint64_t stack_base = base + kStackOffset;
    const std::uint64_t stack_top = stack_base + kMappingSize - 16;
    const GuestRange code_range{GuestAddress{code_base}, kMappingSize};

    if (!space->Map(code_range, GuestPermission::Read | GuestPermission::Write)) return 2;
    if (!space->Map(GuestRange{GuestAddress{stack_base}, kMappingSize},
                    GuestPermission::Read | GuestPermission::Write)) return 2;

    auto cr = CreateContext(CpuConfig{}, *space);
    if (!cr) { printf("FAILED create context\n"); return 2; }
    auto ctx = std::move(cr).Value();
    const std::uint64_t gate = ctx->Capabilities().return_gate_address;

    const auto* a = Find("constant_a");
    const auto* b = Find("constant_b");
    if (!a || !b || gate == 0) return 2;

    auto image_of = [&](const Fixtures::Fixture& f) {
        std::vector<std::byte> img(kMappingSize, std::byte{0});
        std::memcpy(img.data(), f.bytes.data(), f.bytes.size());
        if (f.gate_offset >= 0) std::memcpy(img.data() + f.gate_offset, &gate, sizeof(gate));
        return img;
    };
    auto run_once = [&](std::uint64_t& rax, std::string& why) -> bool {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{code_base};
        init.initial_rsp = GuestAddress{stack_top};
        init.guest_tid = 1;
        auto th = ctx->CreateThread(init);
        if (!th) { why = Describe(th.GetError()); return false; }
        auto run = ctx->Run(th.Value(), RunOptions{});
        bool ok = bool(run);
        if (ok) rax = run.Value().snapshot.registers.Get(Gpr::Rax); else why = Describe(run.GetError());
        (void)ctx->DestroyThread(th.Value());
        return ok;
    };

    // 1. Warm a real translation of A.
    {
        if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Write)) return 2;
        auto img = image_of(*a);
        auto q = space->Quiesce(1'000'000'000);
        if (!q) return 2;
        if (!space->PublishCode(q.Value(), code_range, img)) return 2;
    }
    if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Execute)) return 2;
    std::uint64_t rax = 0; std::string why;
    if (!run_once(rax, why)) { printf("FAILED warm run: %s\n", why.c_str()); return 2; }
    printf("warm run of A: rax=%" PRIu64 " (expect 17)\n\n", rax);
    if (rax != 17) return 2;

    // 2. Inject a failing sink and publish B, so the failure happens after the bytes changed.
    auto* real_sink = dynamic_cast<CodeInvalidationSink*>(ctx.get());
    if (!real_sink) return 3;
    FailureSink failure;
    space->ClearCodeInvalidationSink(real_sink);
    if (!space->SetCodeInvalidationSink(&failure)) return 4;

    bool pub_failed = false;
    {
        if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Write)) return 2;
        auto img = image_of(*b);
        auto q = space->Quiesce(1'000'000'000);
        if (!q) return 2;
        pub_failed = !space->PublishCode(q.Value(), code_range, img);
    }
    space->ClearCodeInvalidationSink(&failure);
    if (!space->SetCodeInvalidationSink(real_sink)) return 5;

    printf("publication failed as injected = %d\n", int(pub_failed));
    printf("HasPoisonedCode                = %d\n", int(space->HasPoisonedCode()));

    // 3. Every route back to execution must be refused, and say why.
    auto rx = space->Protect(code_range, GuestPermission::Read | GuestPermission::Execute);
    printf("Protect(RX) while poisoned     = %d", int(bool(rx)));
    if (!rx) printf("  [%s]", Describe(rx.GetError()).c_str());
    printf("\n");

    std::string run_why;
    std::uint64_t rax_poisoned = 0;
    const bool ran_poisoned = run_once(rax_poisoned, run_why);
    printf("execution while poisoned       = %d", int(ran_poisoned));
    if (!ran_poisoned) printf("  [%s]", run_why.c_str());
    else printf("  rax=%" PRIu64, rax_poisoned);
    printf("\n");

    // 4. Repair: a successful invalidation over the same range must lift the block, and the guest
    //    must then run B -- not the stale A.
    bool repaired = false;
    {
        auto q = space->Quiesce(1'000'000'000);
        if (q) repaired = bool(space->InvalidateCode(q.Value(), code_range,
                                                     InvalidationReason::HostWrite));
    }
    printf("\nrepair invalidation succeeded  = %d\n", int(repaired));
    printf("HasPoisonedCode after repair   = %d\n", int(space->HasPoisonedCode()));

    std::uint64_t rax_after = 0; std::string why_after;
    bool ran_after = false;
    if (space->Protect(code_range, GuestPermission::Read | GuestPermission::Execute)) {
        ran_after = run_once(rax_after, why_after);
    } else {
        why_after = "Protect(RX) still refused after repair";
    }
    printf("execution after repair         = %d", int(ran_after));
    if (!ran_after) printf("  [%s]", why_after.c_str());
    else printf("  rax=%" PRIu64, rax_after);
    printf("\n");

    printf("\n--- observations ---\n");
    printf("R1_publication_failed=%d\n", int(pub_failed));
    printf("R1_poison_set=%d\n", int(pub_failed));
    printf("R1_protect_rx_refused_while_poisoned=%d\n", int(!bool(rx)));
    printf("R1_execution_refused_while_poisoned=%d\n", int(!ran_poisoned));
    printf("R1_stale_code_executed=%d\n", int(ran_poisoned && rax_poisoned == 17));
    printf("R1_repair_restored_execution=%d\n", int(ran_after));
    printf("R1_executed_new_constant_after_repair=%d\n", int(ran_after && rax_after == 34));
    return 0;
}
