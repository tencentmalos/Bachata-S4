// SPDX-License-Identifier: GPL-2.0-or-later
// The guest Gnm shader-binding payload (guest/runtime/gnm/shader.c), compiled
// natively here, against the production encoders it replaces as the HLE route
// runs them (src/core/host_runtime/guest_graphics_hle.cpp): same return value
// and same command-buffer bytes for every size/register/modifier case, plus the
// route table against the NID database and the generated payload's exports.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/aerolib/aerolib.h"
#include "core/host_runtime/guest_gnm_abi.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/gnmdriver/gnmdriver_init.h"
#include "guest_gnm_payload.h"

extern "C" {
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
namespace Gnm = Libraries::GnmDriver;
unsigned checks{}, failures{};
void Check(const std::string& name, bool ok) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name.c_str());
    }
}

using Production = u32 (*)(u32*, u32, const u32*, u32);
using Guest = u32 (*)(u32*, u32, const u32*, u32);
struct Encoder {
    const char* name;
    size_t register_words; // what the HLE route reads (guest_graphics_hle.cpp SHADER words)
    Production production;
    Guest guest;
};

// Uniform (cmdbuf, size, regs, extra) adapters; three-argument encoders ignore extra.
template <auto Fn>
u32 Call3(u32* p, u32 size, const u32* regs, u32) {
    return static_cast<u32>(Fn(p, size, regs));
}
template <auto Fn>
u32 Call4(u32* p, u32 size, const u32* regs, u32 extra) {
    return static_cast<u32>(Fn(p, size, regs, extra));
}

const Encoder kEncoders[] = {
    {"sceGnmSetCsShader", 7, Call3<&Gnm::sceGnmSetCsShader>, Call3<&shad_sceGnmSetCsShader>},
    {"sceGnmSetCsShaderWithModifier", 7, Call4<&Gnm::sceGnmSetCsShaderWithModifier>,
     Call4<&shad_sceGnmSetCsShaderWithModifier>},
    {"sceGnmSetEsShader", 4, Call4<&Gnm::sceGnmSetEsShader>, Call4<&shad_sceGnmSetEsShader>},
    {"sceGnmSetGsShader", 7, Call3<&Gnm::sceGnmSetGsShader>, Call3<&shad_sceGnmSetGsShader>},
    {"sceGnmSetHsShader", 7, Call4<&Gnm::sceGnmSetHsShader>, Call4<&shad_sceGnmSetHsShader>},
    {"sceGnmSetLsShader", 4, Call4<&Gnm::sceGnmSetLsShader>, Call4<&shad_sceGnmSetLsShader>},
    {"sceGnmSetPsShader", 12, Call3<&Gnm::sceGnmSetPsShader>, Call3<&shad_sceGnmSetPsShader>},
    {"sceGnmSetPsShader350", 12, Call3<&Gnm::sceGnmSetPsShader350>,
     Call3<&shad_sceGnmSetPsShader350>},
    {"sceGnmSetVsShader", 7, Call4<&Gnm::sceGnmSetVsShader>, Call4<&shad_sceGnmSetVsShader>},
    {"sceGnmUpdateGsShader", 7, Call3<&Gnm::sceGnmUpdateGsShader>,
     Call3<&shad_sceGnmUpdateGsShader>},
    {"sceGnmUpdateHsShader", 7, Call4<&Gnm::sceGnmUpdateHsShader>,
     Call4<&shad_sceGnmUpdateHsShader>},
    {"sceGnmUpdatePsShader", 12, Call3<&Gnm::sceGnmUpdatePsShader>,
     Call3<&shad_sceGnmUpdatePsShader>},
    {"sceGnmUpdatePsShader350", 12, Call3<&Gnm::sceGnmUpdatePsShader350>,
     Call3<&shad_sceGnmUpdatePsShader350>},
    {"sceGnmUpdateVsShader", 7, Call4<&Gnm::sceGnmUpdateVsShader>,
     Call4<&shad_sceGnmUpdateVsShader>},
};

constexpr u32 kMaxSize = 0x100000;
constexpr u32 kFill = 0x5ee1c0deu;

// The HLE route (guest_graphics_hle.cpp `encoder`): reject null/zero/oversized
// buffers, copy the whole capacity into a guarded scratch, run the production
// encoder, reject any write past `size`, else copy `size` dwords back.
u32 Route(const Encoder& e, std::vector<u32>& buffer, bool null_cmdbuf, u32 size, const u32* regs,
          u32 extra) {
    if (null_cmdbuf || !size || size > kMaxSize)
        return u32(-1);
    std::vector<u32> registers(e.register_words);
    if (regs)
        std::memcpy(registers.data(), regs, e.register_words * 4);
    constexpr u32 guard = 0xa55a9187;
    std::vector<u32> encoded(std::max<u64>(size, 4096) + 64, guard);
    std::memcpy(encoded.data(), buffer.data(), size * 4);
    const u32 result = e.production(encoded.data(), size, regs ? registers.data() : nullptr, extra);
    if (!std::all_of(encoded.begin() + size, encoded.end(), [](u32 v) { return v == guard; }))
        return u32(-1);
    std::memcpy(buffer.data(), encoded.data(), size * 4);
    return result;
}

// The register block the guest reads: only the words the route would read are
// defined; the payload must not look past them (the tail is poisoned so a
// longer read changes the output and fails the comparison).
struct Registers {
    bool null{};
    std::array<u32, 16> words{};
};

void Compare(const Encoder& e, u32 size, const Registers& regs, u32 extra, bool null_cmdbuf,
             std::vector<u32>& expected, std::vector<u32>& actual, size_t window) {
    std::fill(expected.begin(), expected.begin() + window, kFill);
    std::copy(expected.begin(), expected.begin() + window, actual.begin());
    const u32* r = regs.null ? nullptr : regs.words.data();
    const u32 want = Route(e, expected, null_cmdbuf, size, r, extra);
    std::array<u32, 16> guest_words = regs.words;
    for (size_t i = e.register_words; i < guest_words.size(); ++i)
        guest_words[i] = 0xdead0000u + u32(i);
    const u32 got = e.guest(null_cmdbuf ? nullptr : actual.data(), size,
                            regs.null ? nullptr : guest_words.data(), extra);
    const bool same =
        want == got && std::equal(expected.begin(), expected.begin() + window, actual.begin());
    if (!same) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "%s size=0x%x regs=%s regs[1]=0x%x extra=0x%x "
                      "null=%d want=0x%x got=0x%x",
                      e.name, size, regs.null ? "null" : "set", regs.words[1], extra, null_cmdbuf,
                      want, got);
        Check(text, false);
        return;
    }
    Check(e.name, true);
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = std::filesystem::current_path() / "gnm-fastpath-root";
    std::filesystem::create_directories(root);
    Common::FS::InitializeAndroidUserPaths(root);
    Common::Log::Setup("gnm-fastpath-tests");

    // Route table: every NID is the named export in the NID database, and every
    // route has its export in the generated payload.
    for (const auto& route : Core::HostRuntime::GnmFastPath::kRoutes) {
        const auto* entry = Core::AeroLib::FindByNid(route.nid);
        const std::string_view exported = route.export_name;
        Check(std::string("nid ") + route.nid,
              entry && exported.starts_with("shad_") && exported.substr(5) == entry->name);
        const bool present =
            std::any_of(std::begin(GuestGnmPayload::Symbols), std::end(GuestGnmPayload::Symbols),
                        [&](const auto& s) { return exported == s.name; });
        Check(std::string("payload export ") + route.export_name, present);
    }
    Check("payload exports == routes", std::size(GuestGnmPayload::Symbols) ==
                                           std::size(Core::HostRuntime::GnmFastPath::kRoutes));
    Check("IsUserPaEnabled",
          shad_sceGnmIsUserPaEnabled() == u32(Gnm::sceGnmIsUserPaEnabled() ? 1 : 0));

    std::vector<u32> expected(kMaxSize + 64), actual(kMaxSize + 64);
    const u32 sizes[] = {0,    1,    2,    0x10, 0x16, 0x17, 0x18, 0x19, 0x1b,  0x1c,
                         0x1d, 0x1e, 0x1f, 0x26, 0x27, 0x28, 0x29, 0x40, 0x1000};
    const u32 extras[] = {0,          0x40,       0x3c0,      0x1000000,  0x3000000,
                          0x30003c0,  0xfcfffc3f, 0x3000400,  1,          0x80000000,
                          0xffffffff, 0x2,        0x7fffffff, 0x00000c00, 0x03fffc3f};
    std::mt19937 rng(0x6e6d5f66);
    auto random_regs = [&](bool address_ok) {
        Registers r;
        for (auto& w : r.words)
            w = rng();
        r.words[1] = address_ok ? 0 : (rng() | 1);
        return r;
    };

    for (const auto& e : kEncoders) {
        // Deterministic grid: every size x a fixed register set x every modifier.
        std::vector<Registers> fixed;
        fixed.push_back({true, {}});
        fixed.push_back({false, {}});
        Registers ones{false, {}};
        ones.words.fill(0xffffffffu);
        ones.words[1] = 0;
        fixed.push_back(ones);
        Registers bad = ones;
        bad.words[1] = 0x42;
        fixed.push_back(bad);
        for (int i = 0; i < 4; ++i)
            fixed.push_back(random_regs(true));
        fixed.push_back(random_regs(false));
        for (u32 size : sizes)
            for (const auto& regs : fixed)
                for (u32 extra : extras)
                    for (bool null_cmdbuf : {false, true})
                        Compare(e, size, regs, extra, null_cmdbuf, expected, actual,
                                std::min<size_t>(size, 4096) + 64);
        // Random cases: random size near the thresholds, registers and modifier.
        for (int i = 0; i < 4000; ++i) {
            const u32 size = rng() % 0x60;
            const auto regs = (rng() % 8 == 0) ? Registers{true, {}} : random_regs(rng() % 6 != 0);
            const u32 extra = (rng() % 2) ? extras[rng() % std::size(extras)] : u32(rng());
            Compare(e, size, regs, extra, false, expected, actual, size + 64);
        }
        // The size bound of the route: 0x100000 is admitted, 0x100001 is not.
        for (u32 size : {kMaxSize - 1, kMaxSize, kMaxSize + 1})
            Compare(e, size, random_regs(true), 0, false, expected, actual,
                    std::min<size_t>(size, kMaxSize) + 64);
    }

    // The HLE route copies only HwInitPacketSize dwords through its scratch for
    // the hardware-state init encoders (guest_graphics_hle.cpp HWINIT): they
    // must never write past that, whatever size the game passes.
    {
        using Init = u32 (*)(u32*, u32);
        const std::pair<const char*, Init> inits[] = {
            {"DispatchInitDefaultHardwareState", &Gnm::sceGnmDispatchInitDefaultHardwareState},
            {"DrawInitDefaultHardwareState200", &Gnm::sceGnmDrawInitDefaultHardwareState200},
            {"DrawInitDefaultHardwareState350", &Gnm::sceGnmDrawInitDefaultHardwareState350}};
        const u32 bound = Gnm::HwInitPacketSize;
        for (const auto& [name, init] : inits) {
            for (u32 size : {0u, 1u, bound - 1, bound, bound + 1, 0x1000u, 0x10000u, kMaxSize}) {
                const size_t window = std::min<size_t>(size, 0x2000) + 64;
                std::fill(expected.begin(), expected.begin() + window, kFill);
                const u32 result = init(expected.data(), size);
                const bool untouched_tail =
                    std::all_of(expected.begin() + std::min(size, bound), expected.begin() + window,
                                [](u32 v) { return v == kFill; });
                char text[128];
                std::snprintf(text, sizeof(text), "%s size=0x%x bound (result 0x%x)", name, size,
                              result);
                Check(text, untouched_tail && result == (size < bound ? 0u : bound));
            }
        }
    }

    std::printf("gnm fastpath: %u checks, %u failures\n", checks, failures);
    Common::Log::Shutdown();
    return failures ? 1 : 0;
}
