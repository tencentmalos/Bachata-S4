// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "core/host_runtime/guest_rwlock.h"
#include "core/host_runtime/guest_rwlock_diagnostics.h"
using namespace Core::GuestCpu;
using namespace Core::GuestCpu::Hle;
using namespace Core::HostRuntime;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
int main() {
    CHECK(GuestRwlockDiagnostics::Matches("scePthreadRwlockRdlock"));
    CHECK(GuestRwlockDiagnostics::Matches("pthread_rwlockattr_init"));
    CHECK(!GuestRwlockDiagnostics::Matches("scePthreadMutexLock"));
    CHECK(GuestRwlockDiagnostics::ParseSlot("") == 0);
    CHECK(GuestRwlockDiagnostics::ParseSlot("0xf53a7b8") == 0xf53a7b8);
    CHECK(GuestRwlockDiagnostics::ParseSlot("F53A7B8") == 0xf53a7b8);
    for (const auto* invalid : {"0x", "-1", " 12", "123junk", "10000000000000000"})
        CHECK(!GuestRwlockDiagnostics::ParseSlot(invalid));
    AddressSpaceConfig config{};
    config.reservation_size = 65536;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 65536}, GuestPermission::Read | GuestPermission::Write));
    const u64 caller = 0x4e68b87;
    CHECK(space->WriteData(GuestAddress{base + 256}, std::as_bytes(std::span{&caller, 1})));
    HleCallFrame frame{};
    frame.space = space.get();
    frame.registers.Set(Gpr::Rsp, base + 256);
    for (unsigned i = 0; i < 6; ++i)
        frame.registers.Set(kSysVIntegerOrder[i], base + i);
    GuestRwlockDiagnostics trace;
    GuestRwlockDiagnostics::Record record{};
    unsigned emitted{}, invoked{};
    auto emit = [&](const auto& r) {
        record = r;
        ++emitted;
    };
    auto fail = [&](auto& f) {
        ++invoked;
        CHECK(space->Counts().live_pins == 0);
        f.registers.Set(Gpr::Rdi, 1); // evidence must retain the original argument
        f.registers.Set(Gpr::Rax, 0x80020016);
        return Ok();
    };
    CHECK(trace.Call(frame, fail, emit));
    CHECK(invoked == 1 && emitted == 1 && record.args[0] == base && record.args[5] == base + 5);
    CHECK(record.result == 0x80020016 && record.adapter_ok && record.caller_readable &&
          record.caller == caller);
    CHECK(frame.registers.Get(Gpr::Rax) == 0x80020016 && space->Counts().live_pins == 0);
    CHECK(trace.Call(frame, fail, [](const auto&) { throw std::runtime_error("sink failed"); }));
    CHECK(frame.registers.Get(Gpr::Rax) == 0x80020016);
    frame.registers.Set(Gpr::Rsp, 1);
    CHECK(trace.Call(frame, fail, emit));
    CHECK(!record.caller_readable && record.result == 0x80020016);
    auto rejected = [](auto&) -> Status {
        return MakeError(ErrorCategory::InvalidArgument, "probe", "rejected");
    };
    auto rejected_status = trace.Call(frame, rejected, emit);
    CHECK(!rejected_status && !record.adapter_ok);
    CHECK(rejected_status.GetError().detail == "rejected");
    GuestRwlockDiagnostics filtered(base);
    CHECK(filtered.Call(frame, fail, emit));
    CHECK(emitted == 3); // arg0=1, inner still executes but no observation/read
    GuestRwlockDiagnostics budget;
    unsigned success_records{}, error_records{};
    for (unsigned i = 0; i < 1200; ++i) {
        CHECK(budget.Call(
            frame,
            [i](auto& f) {
                f.registers.Set(Gpr::Rax, i < 1000 ? 0 : POSIX_EBUSY);
                return Ok();
            },
            [&](const auto& r) { r.result ? ++error_records : ++success_records; }));
    }
    CHECK(success_records == 32 && error_records == 64);
    // Real domain contention, including reader/writer exclusion and intact
    // protected data. Atomics keep a broken-domain negative result free of UB.
    u64 next = base + 4096;
    GuestRwlockDomain domain(*space, [&] { return next += 64; });
    CHECK(domain.Init(base, 0) == 0);
    GuestRwlockDiagnostics concurrent;
    std::atomic<u64> writer{}, value{}, complement{~u64{0}}, errors{}, records{}, iterations{};
    std::vector<std::jthread> workers;
    for (u64 owner = 1; owner <= 6; ++owner)
        workers.emplace_back([&, owner] {
            HleCallFrame f{};
            f.space = space.get();
            f.registers.Set(Gpr::Rdi, base);
            auto invoke = [&](auto&& operation) {
                int rc{};
                auto status = concurrent.Call(
                    f,
                    [&](auto& input) {
                        rc = operation();
                        input.registers.Set(Gpr::Rax, rc);
                        return Ok();
                    },
                    [&](const auto&) { ++records; });
                if (!status || rc)
                    ++errors;
                return rc;
            };
            for (u64 i = 0; i < 1000; ++i) {
                const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
                if (invoke([&] { return domain.Lock(base, owner, true, false, {}, deadline); }))
                    break;
                if (writer.exchange(owner))
                    ++errors;
                value = owner * 1000 + i;
                complement = ~value.load();
                if (writer.exchange(0) != owner)
                    ++errors;
                invoke([&] { return domain.Unlock(base, owner); });
                if (invoke([&] { return domain.Lock(base, owner, false, false, {}, deadline); }))
                    break;
                if (writer || complement != ~value.load())
                    ++errors;
                invoke([&] { return domain.Unlock(base, owner); });
                ++iterations;
            }
        });
    workers.clear();
    CHECK(errors == 0 && iterations == 6000 && records == 32);
    CHECK(domain.Destroy(base) == 0 && space->Counts().live_pins == 0);
    std::printf("RWLOCK_DIAGNOSTICS checks=%u failures=%u contention_iterations=%llu errors=%llu\n",
                checks, failures, (unsigned long long)iterations.load(),
                (unsigned long long)errors.load());
    return failures ? 1 : 0;
}
