// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// HLE ABI marshalling tests (acceptance H01, H04).
//
// Scope: these drive the adapter directly with a hand-built register frame, so
// they verify SysV argument placement, stack spill, pointer validation and
// refusal behaviour. They do NOT involve FEX, so they do not prove that a real
// guest reaches these functions with these registers.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/hle/call_adapter.h"

namespace {

using namespace Core::GuestCpu;
using namespace Core::GuestCpu::Hle;

int g_failures = 0;
int g_cases = 0;
bool g_ok = true;
std::string g_detail;

void Check(bool condition, const char* what) {
    if (!condition && g_ok) {
        g_ok = false;
        g_detail = what;
    }
}

template <typename Fn>
void RunCase(const char* id, const char* name, Fn&& body) {
    g_ok = true;
    g_detail.clear();
    ++g_cases;
    body();
    if (!g_ok) {
        ++g_failures;
    }
    std::printf("[%-5s] %-56s %s%s%s\n", id, name, g_ok ? "PASS" : "FAIL",
                g_ok ? "" : " -- ", g_detail.c_str());
}

// --- functions under test ---------------------------------------------------

// Eight integers: six in GPRs, two spilled to the guest stack.
std::uint64_t g_sum8_witness[8];
std::uint64_t Sum8(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                   std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    g_sum8_witness[0] = a; g_sum8_witness[1] = b; g_sum8_witness[2] = c;
    g_sum8_witness[3] = d; g_sum8_witness[4] = e; g_sum8_witness[5] = f;
    g_sum8_witness[6] = g; g_sum8_witness[7] = h;
    return a + b + c + d + e + f + g + h;
}

// Nine doubles: eight in xmm0..7, one spilled.
double g_sum9_witness[9];
double Sum9(double a, double b, double c, double d, double e, double f, double g, double h,
            double i) {
    g_sum9_witness[0] = a; g_sum9_witness[1] = b; g_sum9_witness[2] = c;
    g_sum9_witness[3] = d; g_sum9_witness[4] = e; g_sum9_witness[5] = f;
    g_sum9_witness[6] = g; g_sum9_witness[7] = h; g_sum9_witness[8] = i;
    return a + b + c + d + e + f + g + h + i;
}

// Mixed: integer and float classes consume independent register sequences.
std::uint64_t g_mixed_ints[3];
double g_mixed_doubles[2];
double Mixed(std::uint64_t a, double x, std::uint64_t b, double y, std::uint64_t c) {
    g_mixed_ints[0] = a; g_mixed_ints[1] = b; g_mixed_ints[2] = c;
    g_mixed_doubles[0] = x; g_mixed_doubles[1] = y;
    return static_cast<double>(a + b + c) + x + y;
}

std::uint64_t ReadThrough(const std::uint64_t* pointer) {
    return pointer == nullptr ? 0xDEAD : *pointer;
}

void WriteThrough(std::uint64_t* pointer, std::uint64_t value) {
    if (pointer != nullptr) {
        *pointer = value;
    }
}

float ReturnFloat(float value) {
    return value * 2.0f;
}

// Deliberately unsupported: an aggregate parameter must be refused at
// registration rather than reinterpret_cast into a host call.
struct BigAggregate {
    std::uint64_t a, b, c, d;
};
std::uint64_t TakesAggregate(BigAggregate value) {
    return value.a;
}

// --- fixtures ---------------------------------------------------------------

struct Fixture {
    std::unique_ptr<GuestAddressSpace> space;
    GuestAddress stack_base{};
    GuestAddress data_base{};

    static Fixture Make() {
        Fixture fixture;
        AddressSpaceConfig config{};
        config.reservation_size = 16ull * 1024 * 1024;
        fixture.space = std::move(GuestAddressSpace::Create(config)).Value();

        const std::uint64_t page = HostPageSize();
        auto stack = GuestRange::Checked(fixture.space->ReservationBase(), page * 2).Value();
        (void)fixture.space->Map(stack, GuestPermission::Read | GuestPermission::Write);
        // Point RSP into the middle so both the return slot and spilled
        // arguments above it are inside the mapping.
        fixture.stack_base = GuestAddress{stack.base.value + page};

        auto data = GuestRange::Checked(
            GuestAddress{fixture.space->ReservationBase().value + page * 4}, page).Value();
        (void)fixture.space->Map(data, GuestPermission::Read | GuestPermission::Write);
        fixture.data_base = data.base;
        return fixture;
    }

    HleCallFrame Frame(std::uint64_t operation) const {
        HleCallFrame frame{};
        frame.operation = operation;
        frame.space = space.get();
        frame.registers.Set(Gpr::Rsp, stack_base.value);
        frame.registers.validity = RegisterValidity::Gpr | RegisterValidity::Xmm;
        return frame;
    }

    void PokeStack(std::uint64_t slot, std::uint64_t value) const {
        // Slot 0 is the return address; arguments start at slot 1.
        const GuestAddress at{stack_base.value + slot * sizeof(std::uint64_t)};
        std::array<std::byte, sizeof(std::uint64_t)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(value));
        (void)space->Write(at, bytes);
    }

    std::uint64_t PeekGuest(GuestAddress at) const {
        std::array<std::byte, sizeof(std::uint64_t)> bytes{};
        (void)space->Read(at, bytes);
        std::uint64_t value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    }
};

std::uint64_t DoubleBits(double value) {
    std::uint64_t bits{};
    std::memcpy(&bits, &value, sizeof(value));
    return bits;
}

// --- tests ------------------------------------------------------------------

void TestIntegerSpill() {
    RunCase("H01a", "8 integer args: 6 in GPRs, 2 spilled to the guest stack", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&Sum8, "test.sum8");
        Check(op.HasValue(), "registration failed");
        if (!op) return;

        auto frame = fixture.Frame(op.Value());
        frame.registers.Set(Gpr::Rdi, 1);
        frame.registers.Set(Gpr::Rsi, 2);
        frame.registers.Set(Gpr::Rdx, 3);
        frame.registers.Set(Gpr::Rcx, 4);   // 4th argument is rcx in a plain call
        frame.registers.Set(Gpr::R8, 5);
        frame.registers.Set(Gpr::R9, 6);
        fixture.PokeStack(1, 7);
        fixture.PokeStack(2, 8);

        auto status = registry.Dispatch(frame);
        Check(status.HasValue(), "dispatch failed");
        if (!status) {
            g_detail += " (" + Describe(status.GetError()) + ")";
            return;
        }

        for (std::uint64_t i = 0; i < 8; ++i) {
            Check(g_sum8_witness[i] == i + 1, "an argument landed in the wrong slot");
        }
        Check(frame.registers.Get(Gpr::Rax) == 36, "return value mismatch (1+..+8)");
    });

    RunCase("H01b", "4th integer arg survives a syscall-style gate (r10 -> rcx)", [] {
        // Acceptance H01 calls this out explicitly: a syscall callgate clobbers
        // rcx, so the 4th argument arrives in r10 and must be normalised before
        // the adapter reads it.
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&Sum8, "test.sum8.syscall");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        frame.registers.Set(Gpr::Rdi, 10);
        frame.registers.Set(Gpr::Rsi, 20);
        frame.registers.Set(Gpr::Rdx, 30);
        frame.registers.Set(Gpr::R10, 40);   // as delivered by the gate
        frame.registers.Set(Gpr::Rcx, 0xBADBADBAD);  // clobbered by syscall
        frame.registers.Set(Gpr::R8, 50);
        frame.registers.Set(Gpr::R9, 60);
        fixture.PokeStack(1, 70);
        fixture.PokeStack(2, 80);

        // The gate's responsibility, performed here explicitly.
        frame.registers.Set(Gpr::Rcx, frame.registers.Get(Gpr::R10));
        frame.rcx_normalised_from_r10 = true;

        Check(registry.Dispatch(frame).HasValue(), "dispatch failed");
        Check(g_sum8_witness[3] == 40, "4th argument was not taken from r10");
        Check(frame.registers.Get(Gpr::Rax) == 360, "return value mismatch");
    });
}

void TestFloatSpill() {
    RunCase("H01c", "9 double args: 8 in xmm0-7, 1 spilled", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&Sum9, "test.sum9");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        for (std::size_t i = 0; i < 8; ++i) {
            frame.registers.xmm[i] = Xmm{DoubleBits(static_cast<double>(i + 1)), 0};
        }
        fixture.PokeStack(1, DoubleBits(9.0));

        Check(registry.Dispatch(frame).HasValue(), "dispatch failed");
        for (std::size_t i = 0; i < 9; ++i) {
            Check(g_sum9_witness[i] == static_cast<double>(i + 1),
                  "a double landed in the wrong slot");
        }
        double returned{};
        const std::uint64_t bits = frame.registers.xmm[0].low;
        std::memcpy(&returned, &bits, sizeof(returned));
        Check(returned == 45.0, "double return value mismatch (1+..+9)");
    });

    RunCase("H01d", "mixed integer and float use independent register sequences", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&Mixed, "test.mixed");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        // Integers take rdi, rsi, rdx in order; doubles take xmm0, xmm1.
        frame.registers.Set(Gpr::Rdi, 100);
        frame.registers.Set(Gpr::Rsi, 200);
        frame.registers.Set(Gpr::Rdx, 300);
        frame.registers.xmm[0] = Xmm{DoubleBits(0.5), 0};
        frame.registers.xmm[1] = Xmm{DoubleBits(0.25), 0};

        Check(registry.Dispatch(frame).HasValue(), "dispatch failed");
        Check(g_mixed_ints[0] == 100 && g_mixed_ints[1] == 200 && g_mixed_ints[2] == 300,
              "integer arguments misplaced");
        Check(g_mixed_doubles[0] == 0.5 && g_mixed_doubles[1] == 0.25,
              "double arguments misplaced");
        double returned{};
        const std::uint64_t bits = frame.registers.xmm[0].low;
        std::memcpy(&returned, &bits, sizeof(returned));
        Check(returned == 600.75, "mixed return value mismatch");
    });

    RunCase("H01e", "float return uses the low 32 bits of xmm0", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&ReturnFloat, "test.float");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        float input = 1.5f;
        std::uint64_t bits{};
        std::memcpy(&bits, &input, sizeof(input));
        frame.registers.xmm[0] = Xmm{bits, 0};

        Check(registry.Dispatch(frame).HasValue(), "dispatch failed");
        float returned{};
        const std::uint64_t out = frame.registers.xmm[0].low;
        std::memcpy(&returned, &out, sizeof(returned));
        Check(returned == 3.0f, "float return mismatch");
    });
}

void TestPointerValidation() {
    RunCase("H04a", "a valid guest pointer is read through", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&ReadThrough, "test.read");
        if (!op) { Check(false, "registration failed"); return; }

        std::array<std::byte, 8> payload{};
        const std::uint64_t expected = 0x0123456789ABCDEF;
        std::memcpy(payload.data(), &expected, sizeof(expected));
        Check(fixture.space->Write(fixture.data_base, payload).HasValue(), "seed write failed");

        auto frame = fixture.Frame(op.Value());
        frame.registers.Set(Gpr::Rdi, fixture.data_base.value);
        Check(registry.Dispatch(frame).HasValue(), "dispatch failed");
        Check(frame.registers.Get(Gpr::Rax) == expected, "value read through pointer mismatch");
    });

    RunCase("H04b", "a null guest pointer is passed as null, not rejected", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&ReadThrough, "test.read.null");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        frame.registers.Set(Gpr::Rdi, 0);
        Check(registry.Dispatch(frame).HasValue(), "dispatch should succeed with null");
        Check(frame.registers.Get(Gpr::Rax) == 0xDEAD, "callee did not observe null");
    });

    RunCase("H04c", "an unmapped guest pointer is refused, native code not called", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&ReadThrough, "test.read.unmapped");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        // Well above 4096, so the reference's "non-null and large means ok"
        // rule would have let this through to native code.
        frame.registers.Set(Gpr::Rdi, fixture.space->ReservationBase().value + 0x400000);
        frame.registers.Set(Gpr::Rax, 0x5A5A5A5A);

        auto status = registry.Dispatch(frame);
        Check(!status.HasValue(), "an unmapped pointer must be refused");
        Check(frame.registers.Get(Gpr::Rax) == 0x5A5A5A5A,
              "a refused call must not write a return value");
    });

    RunCase("H04d", "a read-only guest pointer is refused for a writable parameter", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&WriteThrough, "test.write");
        if (!op) { Check(false, "registration failed"); return; }

        const std::uint64_t page = HostPageSize();
        auto readonly = GuestRange::Checked(
            GuestAddress{fixture.space->ReservationBase().value + page * 8}, page).Value();
        Check(fixture.space->Map(readonly, GuestPermission::Read).HasValue(),
              "read-only map failed");

        auto frame = fixture.Frame(op.Value());
        frame.registers.Set(Gpr::Rdi, readonly.base.value);
        frame.registers.Set(Gpr::Rsi, 0xFFFF);

        auto status = registry.Dispatch(frame);
        Check(!status.HasValue(), "writing through a read-only pointer must be refused");
        if (!status.HasValue()) {
            Check(status.GetError().category == ErrorCategory::PermissionDenied,
                  "refusal should be PermissionDenied");
        }
    });

    RunCase("H04e", "a writable guest pointer is written through", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&WriteThrough, "test.write.ok");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        frame.registers.Set(Gpr::Rdi, fixture.data_base.value);
        frame.registers.Set(Gpr::Rsi, 0xC0FFEE);
        Check(registry.Dispatch(frame).HasValue(), "dispatch failed");
        Check(fixture.PeekGuest(fixture.data_base) == 0xC0FFEE,
              "value was not written into guest memory");
    });
}

void TestRefusals() {
    RunCase("H04f", "an unsupported signature is refused at registration", [] {
        HleCallRegistry registry;
        auto op = registry.Register(&TakesAggregate, "test.aggregate");
        Check(!op.HasValue(), "an aggregate parameter must be refused");
        if (!op.HasValue()) {
            Check(op.GetError().category == ErrorCategory::Unsupported,
                  "refusal must be Unsupported");
        }
        Check(registry.Size() == 0, "a refused function must not be registered");
    });

    RunCase("H04g", "an unregistered operation is an error, not a host syscall", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        (void)registry.Register(&Sum8, "test.sum8");

        auto frame = fixture.Frame(9999);
        auto status = registry.Dispatch(frame);
        Check(!status.HasValue(), "an unregistered operation must fail");
        if (!status.HasValue()) {
            Check(status.GetError().category == ErrorCategory::InvalidHandle,
                  "refusal must be InvalidHandle");
        }
    });

    RunCase("H04h", "a spilled argument outside guest memory is refused", [] {
        auto fixture = Fixture::Make();
        HleCallRegistry registry;
        auto op = registry.Register(&Sum8, "test.sum8.badstack");
        if (!op) { Check(false, "registration failed"); return; }

        auto frame = fixture.Frame(op.Value());
        // RSP outside any mapping: the two spilled arguments cannot be read.
        frame.registers.Set(Gpr::Rsp, fixture.space->ReservationBase().value + 0x800000);
        auto status = registry.Dispatch(frame);
        Check(!status.HasValue(), "unreadable spilled arguments must be refused");
    });
}

void TestOperationIdentity() {
    RunCase("H04i", "operation ids are distinct and looked up by identity", [] {
        HleCallRegistry registry;
        auto a = registry.Register(&Sum8, "test.a");
        auto b = registry.Register(&Sum9, "test.b");
        Check(a.HasValue() && b.HasValue(), "registration failed");
        if (!a || !b) return;
        Check(a.Value() != b.Value(), "operation ids must be distinct");
        Check(registry.Find(a.Value()) != nullptr, "lookup by id failed");
        Check(registry.Find(a.Value())->Name() == "test.a", "wrong adapter for id");
        Check(registry.Find(12345) == nullptr, "unknown id must not resolve");
        Check(registry.Size() == 2, "registry size mismatch");
    });
}

} // namespace

int main() {
    std::printf("shadPS4 HLE ABI adapter tests\n");
    std::printf("host page size: %llu\n\n",
                static_cast<unsigned long long>(HostPageSize()));

    TestIntegerSpill();
    TestFloatSpill();
    TestPointerValidation();
    TestRefusals();
    TestOperationIdentity();

    std::printf("\n%d/%d passed\n", g_cases - g_failures, g_cases);
    std::printf("\nSCOPE: adapter-level. Verifies SysV placement, spill, pointer validation\n");
    std::printf("       and refusals against a hand-built frame. No FEX involved, so this\n");
    std::printf("       does not prove a real guest arrives with these registers.\n");
    return g_failures == 0 ? 0 : 1;
}
