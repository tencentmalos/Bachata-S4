// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GuestFiber (Android libSceFiber) on a plain buffer standing in for guest memory: the register
// files a guest thread would continue with after each call, and what the calls leave in memory.
// No guest code runs here; the switch itself (the veneer's `ret` on the new stack) is the FEX
// boundary's part and is checked on a device.

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/host_runtime/guest_fiber.h"

using namespace Core::HostRuntime;
using Core::GuestCpu::Gpr;
using Core::GuestCpu::RegisterFile;

namespace {

// Guest memory 0x10000..0x50000; everything else faults.
struct Memory final : GuestFiberMemory {
    static constexpr u64 kBase = 0x10000, kSize = 0x40000;
    std::vector<std::byte> bytes = std::vector<std::byte>(kSize);
    bool In(u64 address, u64 size) const {
        return address >= kBase && size <= kSize && address - kBase <= kSize - size;
    }
    bool Read(u64 address, std::span<std::byte> into) override {
        if (!In(address, into.size()))
            return false;
        std::memcpy(into.data(), bytes.data() + (address - kBase), into.size());
        return true;
    }
    bool Write(u64 address, std::span<const std::byte> from) override {
        if (!In(address, from.size()))
            return false;
        std::memcpy(bytes.data() + (address - kBase), from.data(), from.size());
        return true;
    }
    std::optional<bool> CompareExchange(u64 address, u32 expected, u32 desired) override {
        if (!In(address, 4) || (address & 3))
            return std::nullopt;
        u32 value{};
        std::memcpy(&value, bytes.data() + (address - kBase), 4);
        if (value != expected)
            return false;
        std::memcpy(bytes.data() + (address - kBase), &desired, 4);
        return true;
    }
    template <class T>
    T Get(u64 address) {
        T value{};
        Read(address, std::as_writable_bytes(std::span{&value, 1}));
        return value;
    }
    template <class T>
    void Put(u64 address, const T& value) {
        Write(address, std::as_bytes(std::span{&value, 1}));
    }
    void PutString(u64 address, const char* text) {
        Write(address, std::as_bytes(std::span{text, std::strlen(text) + 1}));
    }
};

constexpr u64 kEntryReturn = 0xdead0000;
constexpr u64 kFiberA = 0x10000, kFiberB = 0x10100, kFiberC = 0x10200;
constexpr u64 kName = 0x10400, kOpt = 0x10480, kOut = 0x10500, kInfo = 0x10600;
constexpr u64 kStackA = 0x20000, kStackB = 0x30000, kStackSize = 0x4000;
constexpr u64 kThreadStack = 0x48008; // a thread's RSP right after its call
constexpr u64 kEntryA = 0x400000, kEntryB = 0x400100, kEntryC = 0x400200;

const char* kInitialize = "hVYD7Ou2pCQ";
const char* kInitializeImpl = "7+OJIpko9RY";
const char* kRun = "a0LLrZWac0M";
const char* kSwitch = "PFT2S-tJ7Uk";
const char* kReturnToThread = "B0ZX2hx9DMw";
const char* kGetSelf = "p+zLIOg27zU";
const char* kFinalize = "JeNX5F-NzQU";
const char* kGetInfo = "uq2Y5BFz0PE";
const char* kStartCheck = "Lcqty+QNWFc";
const char* kStopCheck = "Kj4nXMpnM8Y";
const char* kAttachAndRun = "avfGJ94g36Q";
const char* kFramePointer = "0dy4JtMUcMQ";

RegisterFile Call(u64 rsp, std::initializer_list<u64> args, u64 seed = 0) {
    RegisterFile r{};
    const Gpr order[] = {Gpr::Rdi, Gpr::Rsi, Gpr::Rdx, Gpr::Rcx, Gpr::R8, Gpr::R9};
    size_t i = 0;
    for (u64 a : args)
        r.Set(order[i++], a);
    r.Set(Gpr::Rsp, rsp);
    // Distinct callee-saved values per context.
    r.Set(Gpr::Rbx, seed + 1);
    r.Set(Gpr::Rbp, seed + 2);
    r.Set(Gpr::R12, seed + 3);
    r.Set(Gpr::R13, seed + 4);
    r.Set(Gpr::R14, seed + 5);
    r.Set(Gpr::R15, seed + 6);
    r.mxcsr = 0x1f80;
    return r;
}

s32 Rax(const RegisterFile& r) {
    return static_cast<s32>(static_cast<u32>(r.Get(Gpr::Rax)));
}

} // namespace

int main() {
    unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (false)

    Memory mem;
    GuestFiber fibers{mem, kEntryReturn};
    const GuestFiber::ThreadKey t1{1, 1}, t2{2, 1};
    const auto state = [&](u64 fiber) { return mem.Get<u32>(fiber + 4); };

    // sceFiberInitialize: 7th/8th arguments (option, build version) on the stack.
    mem.PutString(kName, "fiber-a-with-a-name-longer-than-31-chars");
    {
        auto r = Call(kThreadStack, {kFiberA, kName, kEntryA, 0xa1, kStackA, kStackSize});
        mem.Put<u64>(kThreadStack + 8, 0);         // opt_param
        mem.Put<u64>(kThreadStack + 16, 0x4500000); // build: FW 4.50, SetFpuRegs
        CHECK(!fibers.Call(kInitialize, r, t1));
        CHECK(Rax(r) == ORBIS_OK);
        CHECK(state(kFiberA) == GuestFiber::kIdle);
        CHECK(mem.Get<u64>(kStackA) == GuestFiber::kStackSignature);
        char name[32]{};
        mem.Read(kFiberA + 40, std::as_writable_bytes(std::span{name}));
        CHECK(std::strlen(name) == 31 && std::memcmp(name, "fiber-a-with", 12) == 0);
    }
    // Errors are checked before anything is written.
    {
        auto r = Call(kThreadStack, {kFiberB, kName, kEntryB, 0, kStackB + 8, kStackSize});
        CHECK(!fibers.Call(kInitialize, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_ALIGNMENT);
        r = Call(kThreadStack, {kFiberB, kName, kEntryB, 0, kStackB, 256});
        CHECK(!fibers.Call(kInitialize, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_RANGE);
        r = Call(kThreadStack, {kFiberB, 0, kEntryB, 0, kStackB, kStackSize});
        CHECK(!fibers.Call(kInitialize, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_NULL);
        mem.Put<u32>(kOpt, 0x1234);
        r = Call(kThreadStack, {kFiberB, kName, kEntryB, 0, kStackB, kStackSize});
        mem.Put<u64>(kThreadStack + 8, kOpt);
        CHECK(!fibers.Call(kInitialize, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_INVALID);
        CHECK(mem.Get<u32>(kFiberB) == 0);
    }
    // The internal variant: 7th..9th on the stack (option, flags, build version); a valid option.
    {
        auto r = Call(kThreadStack, {kOpt});
        CHECK(!fibers.Call("asjUJJ+aa8s", r, t1) && Rax(r) == ORBIS_OK);
        r = Call(kThreadStack, {kFiberB, kName, kEntryB, 0xb1, kStackB, kStackSize});
        mem.Put<u64>(kThreadStack + 8, kOpt);
        mem.Put<u64>(kThreadStack + 16, 0); // flags
        mem.Put<u64>(kThreadStack + 24, 0x3000000); // build 3.00: no SetFpuRegs
        CHECK(!fibers.Call(kInitializeImpl, r, t1) && Rax(r) == ORBIS_OK);
        CHECK((mem.Get<u32>(kFiberB + 80) & GuestFiber::kFlagSetFpuRegs) == 0);
        CHECK((mem.Get<u32>(kFiberA + 80) & GuestFiber::kFlagSetFpuRegs) != 0);
    }

    // Outside sceFiberRun there is no current fiber.
    {
        auto r = Call(kThreadStack, {kOut});
        CHECK(!fibers.Call(kGetSelf, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_PERMISSION);
        r = Call(kThreadStack, {kFiberB, 0, 0});
        CHECK(!fibers.Call(kSwitch, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_PERMISSION);
        r = Call(kThreadStack, {0, 0});
        CHECK(!fibers.Call(kReturnToThread, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_PERMISSION);
    }

    // sceFiberRun(A): the thread continues at A's entry on A's stack.
    auto thread = Call(kThreadStack, {kFiberA, 0x5150, kOut}, 0x100);
    const auto thread_saved = thread;
    CHECK(!fibers.Call(kRun, thread, t1));
    {
        const u64 top = kStackA + kStackSize;
        CHECK(Rax(thread) == ORBIS_OK);
        CHECK(thread.Get(Gpr::Rsp) == top - 16);
        CHECK(mem.Get<u64>(top - 16) == kEntryA && mem.Get<u64>(top - 8) == kEntryReturn);
        CHECK(thread.Get(Gpr::Rdi) == 0xa1 && thread.Get(Gpr::Rsi) == 0x5150);
        CHECK(thread.Get(Gpr::Rbp) == 0);
        CHECK(thread.mxcsr == GuestFiber::kFiberMxcsr); // SetFpuRegs
        CHECK(state(kFiberA) == GuestFiber::kRun);
    }
    // Inside A: GetSelf, the thread's frame pointer, and a second Run is refused.
    {
        auto r = Call(kStackA + 0x3000, {kOut});
        CHECK(!fibers.Call(kGetSelf, r, t1) && Rax(r) == ORBIS_OK);
        CHECK(mem.Get<u64>(kOut) == kFiberA);
        r = Call(kStackA + 0x3000, {kOut});
        CHECK(!fibers.Call(kFramePointer, r, t1) && mem.Get<u64>(kOut) == 0x102);
        r = Call(kStackA + 0x3000, {kFiberB, 0, 0});
        CHECK(!fibers.Call(kRun, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_PERMISSION);
        r = Call(kStackA + 0x3000, {kFiberA, 0, 0});
        CHECK(!fibers.Call(kFinalize, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_STATE);
    }

    // A switches to B; A is kept and handed back (Idle), B starts.
    const u64 a_arg_on_run = kStackA + 0x2f00;
    auto in_a = Call(kStackA + 0x2f80, {kFiberB, 0x77, a_arg_on_run}, 0x200);
    in_a.mxcsr = 0x1fc0;
    const auto a_saved = in_a;
    CHECK(!fibers.Call(kSwitch, in_a, t1));
    CHECK(Rax(in_a) == ORBIS_OK);
    CHECK(in_a.Get(Gpr::Rsp) == kStackB + kStackSize - 16);
    CHECK(in_a.Get(Gpr::Rdi) == 0xb1 && in_a.Get(Gpr::Rsi) == 0x77);
    CHECK(in_a.mxcsr == 0x1fc0); // B was built for 3.00: MXCSR left as it is
    CHECK(state(kFiberA) == GuestFiber::kIdle && state(kFiberB) == GuestFiber::kRun);

    // B switches back to A, from another thread entirely: A continues in its Switch call.
    const u64 b_arg_on_run = kStackB + 0x3f00;
    {
        auto in_b = Call(kStackB + 0x3f80, {kFiberA, 0x88, b_arg_on_run}, 0x300);
        // Thread 2 is not inside sceFiberRun: refused.
        auto refused = in_b;
        CHECK(!fibers.Call(kSwitch, refused, t2) && Rax(refused) == ORBIS_FIBER_ERROR_PERMISSION);
        CHECK(!fibers.Call(kSwitch, in_b, t1));
        CHECK(Rax(in_b) == ORBIS_OK);
        for (Gpr g : {Gpr::Rsp, Gpr::Rbx, Gpr::Rbp, Gpr::R12, Gpr::R13, Gpr::R14, Gpr::R15})
            CHECK(in_b.Get(g) == a_saved.Get(g));
        CHECK(in_b.mxcsr == 0x1fc0);
        CHECK(mem.Get<u64>(a_arg_on_run) == 0x88);
        CHECK(state(kFiberA) == GuestFiber::kRun && state(kFiberB) == GuestFiber::kIdle);
    }

    // A returns to the thread: the thread continues after its sceFiberRun call.
    const u64 a_resume_arg = kStackA + 0x2e00;
    {
        auto r = Call(kStackA + 0x2e80, {0x99, a_resume_arg}, 0x400);
        CHECK(!fibers.Call(kReturnToThread, r, t1));
        CHECK(Rax(r) == ORBIS_OK);
        for (Gpr g : {Gpr::Rsp, Gpr::Rbx, Gpr::Rbp, Gpr::R12, Gpr::R13, Gpr::R14, Gpr::R15})
            CHECK(r.Get(g) == thread_saved.Get(g));
        CHECK(r.mxcsr == 0x1f80);
        CHECK(mem.Get<u64>(kOut) == 0x99);
        CHECK(state(kFiberA) == GuestFiber::kIdle);
        auto self = Call(kThreadStack, {kOut});
        CHECK(!fibers.Call(kGetSelf, self, t1) && Rax(self) == ORBIS_FIBER_ERROR_PERMISSION);
    }

    // Another thread runs A: it continues in its ReturnToThread call, and B, resumed from A,
    // continues in its Switch call on that thread.
    {
        auto r = Call(0x47008, {kFiberA, 0xaa, kOut}, 0x500);
        CHECK(!fibers.Call(kRun, r, t2));
        CHECK(Rax(r) == ORBIS_OK && r.Get(Gpr::Rsp) == kStackA + 0x2e80);
        CHECK(mem.Get<u64>(a_resume_arg) == 0xaa);
        auto in_a2 = Call(kStackA + 0x2d80, {kFiberB, 0xbb, 0}, 0x600);
        CHECK(!fibers.Call(kSwitch, in_a2, t2));
        CHECK(in_a2.Get(Gpr::Rsp) == kStackB + 0x3f80);
        CHECK(mem.Get<u64>(b_arg_on_run) == 0xbb);
        auto ret = Call(kStackB + 0x3e80, {0xcc, 0}, 0x700);
        CHECK(!fibers.Call(kReturnToThread, ret, t2));
        CHECK(ret.Get(Gpr::Rsp) == 0x47008 && mem.Get<u64>(kOut) == 0xcc);
        CHECK(state(kFiberA) == GuestFiber::kIdle && state(kFiberB) == GuestFiber::kIdle);
    }

    // A fiber without a context borrows the thread's stack and cannot be resumed.
    {
        auto r = Call(kThreadStack, {kFiberC, kName, kEntryC, 0xc1, 0, 0});
        mem.Put<u64>(kThreadStack + 8, 0);
        mem.Put<u64>(kThreadStack + 16, 0x4500000);
        CHECK(!fibers.Call(kInitialize, r, t1) && Rax(r) == ORBIS_OK);
        auto run = Call(kThreadStack, {kFiberC, 1, 0}, 0x800);
        CHECK(!fibers.Call(kRun, run, t1));
        CHECK(run.Get(Gpr::Rsp) == ((kThreadStack & ~u64{15}) - 16));
        CHECK(mem.Get<u64>(run.Get(Gpr::Rsp)) == kEntryC);
        CHECK(mem.Get<u64>(kThreadStack) != kEntryC); // the caller's return slot is untouched
        auto sw = Call(run.Get(Gpr::Rsp) - 0x100, {kFiberA, 2, 0}, 0x900);
        CHECK(!fibers.Call(kSwitch, sw, t1));
        CHECK(sw.Get(Gpr::Rsp) == kStackA + 0x2d80); // A resumes in its Switch to B
        CHECK(state(kFiberC) == GuestFiber::kIdle);
        auto back = Call(kStackA + 0x2c00, {kFiberC, 3, 0}, 0xa00);
        CHECK(!fibers.Call(kSwitch, back, t1));
        CHECK(back.Get(Gpr::Rsp) == ((kThreadStack & ~u64{15}) - 16)); // C starts over
        auto done = Call(back.Get(Gpr::Rsp) - 0x80, {0, 0}, 0xb00);
        CHECK(!fibers.Call(kReturnToThread, done, t1));
        CHECK(done.Get(Gpr::Rsp) == kThreadStack);
    }

    // An overwritten stack signature stops the guest.
    {
        auto run = Call(kThreadStack, {kFiberB, 0, 0}, 0xc00);
        CHECK(!fibers.Call(kRun, run, t1)); // B resumes from its ReturnToThread
        mem.Put<u64>(kStackB, 0);
        auto sw = Call(kStackB + 0x100, {kFiberA, 0, 0}, 0xd00);
        CHECK(fibers.Call(kSwitch, sw, t1).has_value());
    }

    // Context size check: fill, margin, and AttachContextAndRun.
    {
        Memory m2;
        GuestFiber f2{m2, kEntryReturn};
        auto r = Call(kThreadStack, {0});
        CHECK(!f2.Call(kStartCheck, r, t1) && Rax(r) == ORBIS_OK);
        r = Call(kThreadStack, {0});
        CHECK(!f2.Call(kStartCheck, r, t1) && Rax(r) == ORBIS_FIBER_ERROR_STATE);
        m2.PutString(kName, "sized");
        r = Call(kThreadStack, {kFiberA, kName, kEntryA, 0, 0, 0});
        m2.Put<u64>(kThreadStack + 8, 0);
        m2.Put<u64>(kThreadStack + 16, 0x4500000);
        CHECK(!f2.Call(kInitialize, r, t1) && Rax(r) == ORBIS_OK);
        auto run = Call(kThreadStack, {kFiberA, kStackA, 0x1000, 5, 0}, 0xe00);
        CHECK(!f2.Call(kAttachAndRun, run, t1) && Rax(run) == ORBIS_OK);
        CHECK(run.Get(Gpr::Rsp) == kStackA + 0x1000 - 16);
        CHECK(m2.Get<u64>(kStackA + 8) == GuestFiber::kStackSizeCheck);
        m2.Put<u64>(kStackA + 0x800, 1); // the fiber used its stack down to here
        m2.Put<u64>(kInfo, sizeof(GuestFiber::Info));
        auto info = Call(kThreadStack, {kFiberA, kInfo});
        CHECK(!f2.Call(kGetInfo, info, t1) && Rax(info) == ORBIS_OK);
        CHECK(m2.Get<u64>(kInfo + 8) == kEntryA);
        CHECK(m2.Get<u64>(kInfo + 32) == 0x1000);
        CHECK(m2.Get<u64>(kInfo + 72) == 0x800 - 8);
        r = Call(kThreadStack, {});
        CHECK(!f2.Call(kStopCheck, r, t1) && Rax(r) == ORBIS_OK);
        m2.Put<u64>(kInfo, 64);
        info = Call(kThreadStack, {kFiberA, kInfo});
        CHECK(!f2.Call(kGetInfo, info, t1) && Rax(info) == ORBIS_FIBER_ERROR_INVALID);
    }

    // Finalize only an idle fiber, once.
    {
        auto r = Call(kThreadStack, {kFiberC});
        CHECK(!fibers.Call(kFinalize, r, t2) && Rax(r) == ORBIS_OK);
        CHECK(state(kFiberC) == GuestFiber::kTerminated);
        r = Call(kThreadStack, {kFiberC});
        CHECK(!fibers.Call(kFinalize, r, t2) && Rax(r) == ORBIS_FIBER_ERROR_STATE);
        r = Call(kThreadStack, {kFiberC, 0, 0});
        CHECK(!fibers.Call(kRun, r, t2) && Rax(r) == ORBIS_FIBER_ERROR_STATE);
        r = Call(kThreadStack, {0x12345, 0, 0});
        CHECK(!fibers.Call(kRun, r, t2) && Rax(r) == ORBIS_FIBER_ERROR_ALIGNMENT);
    }

    std::printf("guest_fiber_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
