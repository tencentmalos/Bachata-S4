// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// P1-C reproduction: does the PUBLIC GuestAddressSpace publication path actually stop stale
// translated code from executing?
//
// The guest execution harness's LoadFixture calls CpuContext::InvalidateCode -- the backend's own
// entry point. The public GuestAddressSpace::InvalidateCode only bumped a counter. This probe uses
// ONLY the public memory API, the way an embedder would.
//
// Observation values, not a pass/fail suite: exit 0 means the probe completed.

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

const Fixtures::Fixture* Find(std::string_view name) {
    for (const auto& f : Fixtures::kAll) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

class FailureSink final : public CodeInvalidationSink {
public:
 std::string_view Name() const override {return "review-failure";}
 Status DiscardTranslations(GuestRange, InvalidationReason) override {
  return MakeError(ErrorCategory::BackendFailure, "review-failure", "injected invalidation failure");
 }
};
int main() {
    printf("P1-C probe: public publication vs real FEX invalidation\n");
    printf("host page size: %ld\n\n", ::sysconf(_SC_PAGESIZE));

    AddressSpaceConfig cfg{};
    cfg.reservation_size = std::uint64_t{1} << 28;
    cfg.max_address = QueryBackendCapabilities().max_guest_address;
    auto space_r = GuestAddressSpace::Create(cfg);
    if (!space_r) { printf("FAILED: create space\n"); return 2; }
    auto space = std::move(space_r).Value();

    const std::uint64_t base = space->ReservationBase().value;
    const std::uint64_t code_base = base + kCodeOffset;
    const std::uint64_t stack_base = base + kStackOffset;
    const std::uint64_t stack_top = stack_base + kMappingSize - 16;
    const GuestRange code_range{GuestAddress{code_base}, kMappingSize};

    if (!space->Map(code_range, GuestPermission::Read | GuestPermission::Write)) {
        printf("FAILED: map code\n"); return 2;
    }
    if (!space->Map(GuestRange{GuestAddress{stack_base}, kMappingSize},
                    GuestPermission::Read | GuestPermission::Write)) {
        printf("FAILED: map stack\n"); return 2;
    }

    auto ctx_r = CreateContext(CpuConfig{}, *space);
    if (!ctx_r) { printf("FAILED: create context\n"); return 2; }
    auto ctx = std::move(ctx_r).Value();
    const std::uint64_t gate = ctx->Capabilities().return_gate_address;
    if (gate == 0) { printf("FAILED: no return gate\n"); return 2; }

    const auto* a = Find("constant_a");
    const auto* b = Find("constant_b");
    if (!a || !b) { printf("FAILED: fixtures constant_a/constant_b missing\n"); return 2; }

    auto write_fixture = [&](const Fixtures::Fixture& f) -> bool {
        if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Write)) return false;
        {
            auto pin = space->AcquirePinnedSpan(code_range, true);
            if (!pin) return false;
            auto bytes = pin.Value().WritableBytes();
            std::memset(bytes.data(), 0, bytes.size());
            std::memcpy(bytes.data(), f.bytes.data(), f.bytes.size());
            if (f.gate_offset >= 0) std::memcpy(bytes.data() + f.gate_offset, &gate, sizeof(gate));
        }
        return bool(space->Protect(code_range, GuestPermission::Read | GuestPermission::Execute));
    };

    auto run_once = [&](std::uint64_t& rax) -> bool {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{code_base};
        init.initial_rsp = GuestAddress{stack_top};
        init.guest_tid = 1;
        auto th = ctx->CreateThread(init);
        if (!th) { printf("  (CreateThread refused: %s)\n", Describe(th.GetError()).c_str()); return false; }
        auto run = ctx->Run(th.Value(), RunOptions{});
        bool ok = bool(run);
        if (ok) rax = run.Value().snapshot.registers.Get(Gpr::Rax);
        else printf("  (Run refused: %s)\n", Describe(run.GetError()).c_str());
        (void)ctx->DestroyThread(th.Value());
        return ok;
    };

    // --- step 1: warm a translation of A through the real invalidation path ---
    if (!write_fixture(*a)) { printf("FAILED: write A\n"); return 2; }
    {
        auto q = space->Quiesce(1'000'000'000);
        if (!q) { printf("FAILED: quiesce for warm-up\n"); return 2; }
        if (!ctx->InvalidateCode(q.Value(), code_range, InvalidationReason::HostWrite)) {
            printf("FAILED: context invalidate for warm-up\n"); return 2;
        }
    }
    std::uint64_t rax_a = 0;
    if (!run_once(rax_a)) { printf("FAILED: warm run\n"); return 2; }
    printf("warm run of constant_a: rax=%" PRIu64 " (expect 17)\n\n", rax_a);
    if (rax_a != 17) { printf("FAILED: fixture A did not return 17\n"); return 2; }

    // --- step 2: publish B using ONLY the public GuestAddressSpace API --------
    if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Write)) {
        printf("FAILED: protect RW\n"); return 2;
    }
    auto q = space->Quiesce(1'000'000'000);
    if (!q) { printf("FAILED: quiesce for publication\n"); return 2; }

    std::vector<std::byte> image(kMappingSize, std::byte{0});
    std::memcpy(image.data(), b->bytes.data(), b->bytes.size());
    if (b->gate_offset >= 0) std::memcpy(image.data() + b->gate_offset, &gate, sizeof(gate));

    const std::uint64_t gen_before = space->CodeGeneration();
    auto* real_sink = dynamic_cast<CodeInvalidationSink*>(ctx.get());
    if (!real_sink) return 3;
    FailureSink failure;
    space->ClearCodeInvalidationSink(real_sink);
    if (!space->SetCodeInvalidationSink(&failure)) return 4;
    auto pub = space->PublishCode(q.Value(), code_range, image);
    printf("REVIEW_failed_publication=%d poison=%d\n", !bool(pub), space->HasPoisonedCode());
    space->ClearCodeInvalidationSink(&failure);
    if (!space->SetCodeInvalidationSink(real_sink)) return 5;
    // No successful invalidation is performed after the injected failure.
    const std::uint64_t gen_after = space->CodeGeneration();

    printf("public PublishCode accepted   = %d\n", int(bool(pub)));
    printf("REVIEW_no_followup_invalidation=1\n");
    printf("code_generation %" PRIu64 " -> %" PRIu64 "\n", gen_before, gen_after);

    // --- step 3: release the transaction, then execute ------------------------
    { auto released = std::move(q).Value(); (void)released; }
    if (!space->Protect(code_range, GuestPermission::Read | GuestPermission::Execute)) {
        printf("FAILED: protect RX\n"); return 2;
    }
    printf("REVIEW_Protect_RX_accepted_while_poisoned=%d\n", space->HasPoisonedCode());
    std::uint64_t rax_b = 0;
    if (!run_once(rax_b)) { printf("FAILED: post-publication run\n"); return 2; }
    printf("\npost-publication run: rax=%" PRIu64 " (34 = new B, 17 = STALE A)\n", rax_b);

    printf("\n--- observations ---\n");
    printf("P1C_publish_accepted=%d\n", int(bool(pub)));

    printf("P1C_generation_advanced=%d\n", int(gen_after > gen_before));
    printf("P1C_executed_rax=%" PRIu64 "\n", rax_b);
    printf("P1C_stale_code_executed=%d\n", int(rax_b == 17));
    return 0;
}
