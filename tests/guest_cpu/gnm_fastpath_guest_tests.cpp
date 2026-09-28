// SPDX-License-Identifier: GPL-2.0-or-later
// Real FEX execution of the production guest Gnm shader-binding payload
// (guest/runtime/gnm/shader.c) with a small guest driver: every case must give
// the same return value and command-buffer bytes as the same source compiled
// natively (which host_runtime/guest_gnm_fastpath_tests checks against the
// production encoders). Also times the warmed guest calls.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#include "core/guest_cpu/fex/fex_context.h"
#include "gnm_fastpath_guest_payload.h"
using namespace Core::GuestCpu;

extern "C" {
using u32 = uint32_t;
using s32 = int32_t;
s32 shad_sceGnmSetCsShader(u32*, u32, const u32*);
s32 shad_sceGnmSetCsShaderWithModifier(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetEsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetGsShader(u32*, u32, const u32*);
s32 shad_sceGnmSetHsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetLsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetPsShader(u32*, u32, const u32*);
s32 shad_sceGnmSetPsShader350(u32*, u32, const u32*);
s32 shad_sceGnmSetVsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmUpdateGsShader(u32*, u32, const u32*);
s32 shad_sceGnmUpdateHsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmUpdatePsShader(u32*, u32, const u32*);
s32 shad_sceGnmUpdatePsShader350(u32*, u32, const u32*);
s32 shad_sceGnmUpdateVsShader(u32*, u32, const u32*, u32);
u32 shad_sceGnmIsUserPaEnabled(void);
}

namespace {
unsigned checks{}, failures{};
void Check(const char* name, bool ok) {
    ++checks;
    failures += !ok;
    std::printf("[GF%02u] %s %s\n", checks, ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
}
template <class T>
T Must(Result<T> r) {
    if (!r)
        throw std::runtime_error(Describe(r.GetError()));
    return std::move(r).Value();
}
void Must(Status r) {
    if (!r)
        throw std::runtime_error(Describe(r.GetError()));
}

// Same layout as the guest driver (tests/guest_cpu/gnm_fastpath_guest/entry.c).
struct Case {
    uint64_t fn, cmdbuf, size, regs, extra, result;
};
struct Batch {
    uint64_t count, repeat, cases;
};

constexpr unsigned kFunctions = 15;
const char* const kNames[kFunctions] = {"SetCsShader",       "SetCsShaderWithModifier",
                                        "SetEsShader",       "SetGsShader",
                                        "SetHsShader",       "SetLsShader",
                                        "SetPsShader",       "SetPsShader350",
                                        "SetVsShader",       "UpdateGsShader",
                                        "UpdateHsShader",    "UpdatePsShader",
                                        "UpdatePsShader350", "UpdateVsShader",
                                        "IsUserPaEnabled"};

uint32_t Native(unsigned fn, u32* p, u32 size, const u32* r, u32 x) {
    switch (fn) {
    case 0:
        return u32(shad_sceGnmSetCsShader(p, size, r));
    case 1:
        return u32(shad_sceGnmSetCsShaderWithModifier(p, size, r, x));
    case 2:
        return u32(shad_sceGnmSetEsShader(p, size, r, x));
    case 3:
        return u32(shad_sceGnmSetGsShader(p, size, r));
    case 4:
        return u32(shad_sceGnmSetHsShader(p, size, r, x));
    case 5:
        return u32(shad_sceGnmSetLsShader(p, size, r, x));
    case 6:
        return u32(shad_sceGnmSetPsShader(p, size, r));
    case 7:
        return u32(shad_sceGnmSetPsShader350(p, size, r));
    case 8:
        return u32(shad_sceGnmSetVsShader(p, size, r, x));
    case 9:
        return u32(shad_sceGnmUpdateGsShader(p, size, r));
    case 10:
        return u32(shad_sceGnmUpdateHsShader(p, size, r, x));
    case 11:
        return u32(shad_sceGnmUpdatePsShader(p, size, r));
    case 12:
        return u32(shad_sceGnmUpdatePsShader350(p, size, r));
    case 13:
        return u32(shad_sceGnmUpdateVsShader(p, size, r, x));
    default:
        return shad_sceGnmIsUserPaEnabled();
    }
}

constexpr unsigned kCases = 1024;    // per batch
constexpr unsigned kSlotWords = 128; // command buffer per case; encoders write <= 40
constexpr unsigned kRegWords = 16;
constexpr u32 kFill = 0x5ee1c0deu;

constexpr unsigned kRegions = 4; // one per concurrently running guest thread

struct Region {
    uint64_t batch, cases, cmdbufs, regs, stack;
};

struct Harness {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> cpu;
    uint64_t base{}, code{};
    std::array<Region, kRegions> regions{};
    Harness() {
        AddressSpaceConfig cfg{};
        cfg.reservation_size = 1ull << 28;
        cfg.max_address = QueryBackendCapabilities().max_guest_address;
        space = Must(GuestAddressSpace::Create(cfg));
        base = space->ReservationBase().value;
        code = base + 0x10000;
        const auto rw = GuestPermission::Read | GuestPermission::Write;
        Must(space->Map({GuestAddress{code}, 0x4000}, rw));
        for (unsigned t = 0; t < kRegions; ++t) {
            auto& r = regions[t];
            const uint64_t at = base + 0x100000 + t * 0x800000ull;
            r.batch = at;
            r.cases = at + 0x1000;
            r.cmdbufs = at + 0x100000;
            r.regs = at + 0x200000;
            r.stack = at + 0x300000;
            Must(space->Map({GuestAddress{r.batch}, 0xff000}, rw));
            Must(space->Map({GuestAddress{r.cmdbufs}, kCases * kSlotWords * 4}, rw));
            Must(space->Map({GuestAddress{r.regs}, kCases * kRegWords * 4}, rw));
            Must(space->Map({GuestAddress{r.stack}, 0x10000}, rw));
        }
        if (sizeof(GnmFastPathGuest::Image) > 0x4000)
            throw std::runtime_error("payload image too large");
        Must(space->Write(GuestAddress{code}, std::as_bytes(std::span{GnmFastPathGuest::Image})));
        Must(space->Protect({GuestAddress{code}, 0x4000},
                            GuestPermission::Read | GuestPermission::Execute));
        cpu = Must(CreateContext({}, *space));
    }
    template <class T>
    void Put(uint64_t va, std::span<const T> x) {
        Must(space->Write(GuestAddress{va}, std::as_bytes(x)));
    }
    template <class T>
    void Get(uint64_t va, std::span<T> x) {
        Must(space->Read(GuestAddress{va}, std::as_writable_bytes(x)));
    }
    GuestCallResult Run(unsigned t, uint64_t count, uint64_t repeat) {
        const auto& r = regions[t];
        const Batch b{count, repeat, r.cases};
        Put(r.batch, std::span{&b, 1});
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{code + GnmFastPathGuest::EntryOffset};
        init.initial_rsp = GuestAddress{r.stack + 0xfff0};
        init.guest_tid = 100 + t;
        auto thread = Must(cpu->CreateThread(init));
        GuestCallArgs args{};
        args.count = 1;
        args.values[0] = r.batch;
        auto result = cpu->InvokeGuest(thread, init.entry_rip, args, {});
        Must(cpu->DestroyThread(thread));
        return Must(std::move(result));
    }
};

struct Input {
    unsigned fn;
    bool null_cmdbuf, null_regs;
    u32 size, extra;
    std::array<u32, kRegWords> words;
};
} // namespace

// Runs every input once on guest thread `t` (region t) and compares each case
// with the native build. With `shifted`, slot bases move by 0..3 dwords so the
// encoders' merged stores land at every 4-byte phase of a 16-byte granule.
unsigned RunInputs(Harness& h, unsigned t, const std::vector<Input>& inputs, bool shifted,
                   unsigned* compared_out, bool* returned_out) {
    const auto& r = h.regions[t];
    unsigned mismatches = 0, compared = 0;
    bool returned = true;
    std::vector<Case> batch(kCases);
    std::vector<u32> slots(kCases * kSlotWords), guest_slots(kCases * kSlotWords);
    std::vector<u32> reg_block(kCases * kRegWords);
    for (size_t start = 0; start < inputs.size(); start += kCases) {
        const size_t n = std::min<size_t>(kCases, inputs.size() - start);
        std::fill(slots.begin(), slots.end(), kFill);
        for (size_t i = 0; i < n; ++i) {
            const auto& in = inputs[start + i];
            const size_t shift = shifted ? (i + start / kCases) % 4 : 0;
            std::copy(in.words.begin(), in.words.end(), reg_block.begin() + i * kRegWords);
            batch[i] = {in.fn,    in.null_cmdbuf ? 0 : r.cmdbufs + (i * kSlotWords + shift) * 4,
                        in.size,  in.null_regs ? 0 : r.regs + i * kRegWords * 4,
                        in.extra, 0xfeedfaceull};
        }
        h.Put(r.cases, std::span<const Case>{batch.data(), n});
        h.Put(r.cmdbufs, std::span<const u32>{slots});
        h.Put(r.regs, std::span<const u32>{reg_block});
        const auto run = h.Run(t, n, 1);
        returned &= run.reason == StopReason::Returned && run.return_value == n;
        h.Get(r.cases, std::span<Case>{batch.data(), n});
        h.Get(r.cmdbufs, std::span<u32>{guest_slots});
        for (size_t i = 0; i < n; ++i) {
            const auto& in = inputs[start + i];
            const size_t shift = shifted ? (i + start / kCases) % 4 : 0;
            u32* slot = slots.data() + i * kSlotWords;
            const u32 want =
                Native(in.fn, in.null_cmdbuf ? nullptr : slot + shift, in.size,
                       in.null_regs ? nullptr : reg_block.data() + i * kRegWords, in.extra);
            const bool same =
                batch[i].result == want &&
                std::equal(slot, slot + kSlotWords, guest_slots.data() + i * kSlotWords);
            ++compared;
            if (!same && mismatches++ < 4)
                std::printf(
                    "  t%u mismatch %s size=0x%x shift=%zu extra=0x%x want=0x%x got=0x%llx\n", t,
                    kNames[in.fn], in.size, shift, in.extra, want,
                    (unsigned long long)batch[i].result);
        }
    }
    *compared_out += compared;
    *returned_out = *returned_out && returned;
    return mismatches;
}

int main() {
    try {
        Harness h;
        std::printf("gnm payload image=%zu entry=%llu\n", sizeof(GnmFastPathGuest::Image),
                    (unsigned long long)GnmFastPathGuest::EntryOffset);
        std::mt19937 rng(0x67666d66);
        const u32 sizes[] = {0,    1,    0x16, 0x17, 0x18, 0x19, 0x1b,   0x1c,     0x1d,    0x1e,
                             0x1f, 0x26, 0x27, 0x28, 0x29, 0x40, 0x1000, 0x100000, 0x100001};
        const u32 extras[] = {0,          0x40,       0x3c0,     0x1000000, 0x3000000,
                              0x30003c0,  0xfcfffc3f, 0x3000400, 1,         0x80000000,
                              0xffffffff, 0x00000c00, 0x03fffc3f};
        std::vector<Input> inputs;
        for (unsigned fn = 0; fn < kFunctions; ++fn) {
            for (int i = 0; i < 1400; ++i) {
                Input in{};
                in.fn = fn;
                in.size = (i % 3 == 0) ? sizes[rng() % std::size(sizes)] : rng() % 0x60;
                in.extra = (rng() % 2) ? extras[rng() % std::size(extras)] : u32(rng());
                in.null_cmdbuf = rng() % 16 == 0;
                in.null_regs = rng() % 10 == 0;
                for (auto& w : in.words)
                    w = rng();
                if (rng() % 6 != 0)
                    in.words[1] = 0;
                if (rng() % 8 == 0)
                    in.words.fill(i % 2 ? 0u : 0xffffffffu), in.words[1] = 0;
                inputs.push_back(in);
            }
        }

        unsigned compared = 0;
        bool returned = true;
        unsigned mismatches = RunInputs(h, 0, inputs, false, &compared, &returned);
        std::printf("aligned: compared=%u mismatches=%u\n", compared, mismatches);
        Check("FEX-executed payload == native build, 16-byte aligned slots",
              returned && mismatches == 0 && compared == inputs.size());

        compared = 0;
        mismatches = RunInputs(h, 0, inputs, true, &compared, &returned);
        std::printf("shifted: compared=%u mismatches=%u\n", compared, mismatches);
        Check("same with slots at every dword phase", returned && mismatches == 0);

        // Several guest threads at once (each its own region), repeatedly: the
        // multi-threaded case of FEX's unaligned-access backpatching.
        std::array<unsigned, kRegions> mt_mismatch{}, mt_compared{};
        std::array<bool, kRegions> mt_returned{};
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < kRegions; ++t)
            workers.emplace_back([&, t] {
                mt_returned[t] = true;
                for (int pass = 0; pass < 12; ++pass)
                    mt_mismatch[t] +=
                        RunInputs(h, t, inputs, true, &mt_compared[t], &mt_returned[t]);
            });
        for (auto& w : workers)
            w.join();
        unsigned total_mismatch = 0, total_compared = 0;
        bool all_returned = true;
        for (unsigned t = 0; t < kRegions; ++t) {
            total_mismatch += mt_mismatch[t];
            total_compared += mt_compared[t];
            all_returned &= mt_returned[t];
        }
        std::printf("threads=%u compared=%u mismatches=%u\n", kRegions, total_compared,
                    total_mismatch);
        Check("same from 4 concurrent guest threads, 12 passes each",
              all_returned && total_mismatch == 0 &&
                  total_compared == 12 * kRegions * inputs.size());

        // Warmed cost of a typical draw's binds: Vs + Ps with valid registers.
        const auto& r = h.regions[0];
        std::vector<Case> batch(kCases);
        std::vector<u32> reg_block(kCases * kRegWords, 0);
        const size_t bench = 256;
        for (size_t i = 0; i < bench; ++i)
            batch[i] = {i % 2 ? 6u : 8u,
                        r.cmdbufs + i * kSlotWords * 4,
                        0x40,
                        r.regs + i * kRegWords * 4,
                        0,
                        0};
        h.Put(r.regs, std::span<const u32>{reg_block});
        h.Put(r.cases, std::span<const Case>{batch.data(), bench});
        (void)h.Run(0, bench, 10);
        const auto begin = std::chrono::steady_clock::now();
        const auto run = h.Run(0, bench, 400);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
        Check("bench run returned", run.reason == StopReason::Returned);
        std::printf("BENCH calls=%zu elapsed_ns=%lld ns_per_call=%.1f (includes one InvokeGuest)\n",
                    bench * 400, (long long)ns, double(ns) / double(bench * 400));
    } catch (const std::exception& e) {
        std::printf("FATAL %s\n", e.what());
        return 2;
    }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
