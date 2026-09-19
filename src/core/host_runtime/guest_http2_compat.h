// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include "common/types.h"

namespace Core::HostRuntime {
// This is a content compatibility entry, not a generic PLT pattern scan.
// Identity: CUSA50828 01.08, exact decoded executable PT_LOAD[0] SHA-256.
// Restore the original indirect import form, never inject an HTTP result or
// alter a source file. Apply before relocation/final permissions/any execution.
inline constexpr std::string_view Http2CompatTextSha =
    "6764ba967816a66e9d280f4d1d3496ff9f7e215c834b489a5b73be16c8dff9c6";
inline constexpr u64 Http2CompatTextSize = 27475088;
inline constexpr u64 Http2CompatStart = 0x15c0540;
inline constexpr u64 Http2CompatSize = 0x166;
struct Http2ImportBinding {
    u64 slot, got;
    std::string_view nid;
};
inline constexpr Http2ImportBinding Http2CompatBindings[]{
    {0x15c0540, 0x1d3b518, "-HIO4VT87v8"}, {0x15c0560, 0x1d3b528, "izvHhqgDt44"},
    {0x15c0570, 0x1d3b530, "9XYJwCf3lEA"}, {0x15c0580, 0x1d3b538, "mmyOCxQMVYQ"},
    {0x15c0590, 0x1d3b540, "A+NVAFu4eCg"}, {0x15c05a0, 0x1d3b548, "ACjtE27aErY"},
    {0x15c05b0, 0x1d3b550, "b9AvoIaOuHI"}, {0x15c05c0, 0x1d3b558, "nrPfOE8TQu0"},
    {0x15c05d0, 0x1d3b560, "FSAFOzi0FpM"}, {0x15c05e0, 0x1d3b568, "bGN-6zbo7ms"},
    {0x15c05f0, 0x1d3b570, "MOp-AUhdfi8"}, {0x15c0600, 0x1d3b578, "uRosf8GQbHQ"},
    {0x15c0610, 0x1d3b580, "+wCt7fCijgk"}, {0x15c0620, 0x1d3b588, "3JCe3lCbQ8A"},
    {0x15c0640, 0x1d3b598, "pDom5-078DA"}, {0x15c0650, 0x1d3b5a0, "YiBUtz-pGkc"},
    {0x15c0660, 0x1d3b5a8, "c8D9qIjo8EY"}, {0x15c0670, 0x1d3b5b0, "VYMxTcBqSE0"},
    {0x15c0680, 0x1d3b5b8, "XPtW45xiLHk"}, {0x15c0690, 0x1d3b5c0, "n8hMLe31OPA"},
    {0x15c06a0, 0x1d3b5c8, "IZ-qjhRqvjk"},
};
// Validate everything before changing even one byte. The caller independently
// validates ELF segment bounds, R_X86_64_JUMP_SLOT and exact library/module NIDs.
inline bool RestoreHttp2ImportBytes(std::string_view text_sha, std::span<u8> region,
                                    std::span<const Http2ImportBinding> bindings) {
    if (text_sha != Http2CompatTextSha)
        return false;
    if (region.size() != Http2CompatSize || bindings.size() != std::size(Http2CompatBindings))
        throw std::runtime_error("HTTP2 content compatibility extent/relocation mismatch");
    constexpr std::array<u8, 6> before{0x31, 0xc0, 0xc3, 0xaf, 0x77, 0x00};
    for (const auto& expected : Http2CompatBindings) {
        size_t found{};
        for (const auto& actual : bindings)
            found += actual.slot == expected.slot && actual.got == expected.got &&
                     actual.nid == expected.nid;
        if (found != 1 || std::memcmp(region.data() + expected.slot - Http2CompatStart,
                                      before.data(), before.size()))
            throw std::runtime_error("HTTP2 content compatibility bytes/import mismatch");
    }
    for (const auto& entry : Http2CompatBindings) {
        auto* bytes = region.data() + entry.slot - Http2CompatStart;
        const s32 displacement = s32(entry.got - (entry.slot + 6));
        bytes[0] = 0xff;
        bytes[1] = 0x25;
        std::memcpy(bytes + 2, &displacement, sizeof(displacement));
    }
    return true;
}
} // namespace Core::HostRuntime
