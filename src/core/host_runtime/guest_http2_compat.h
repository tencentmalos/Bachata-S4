// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
// Not <openssl/sha.h>: it defines a U64 macro that breaks unity builds sharing this header.
#include <openssl/evp.h>
#include "common/types.h"
#include "core/loader/elf.h"

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

// Lower-case hex SHA-256 of the decoded executable PT_LOAD[0], before any relocation or patch.
inline std::string Http2CompatDigest(std::span<const u8> text) {
    std::array<u8, EVP_MAX_MD_SIZE> hash{};
    unsigned int length{};
    if (!EVP_Digest(text.data(), text.size(), hash.data(), &length, EVP_sha256(), nullptr))
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string digest;
    for (unsigned int i = 0; i < length; ++i) {
        digest += hex[hash[i] >> 4];
        digest += hex[hash[i] & 15];
    }
    return digest;
}

// Whether `segment` is the executable text PT_LOAD this entry is about (checked before hashing).
template <class ProgramHeader>
bool IsHttp2CompatTextSegment(const ProgramHeader& segment) {
    return segment.p_type == PT_LOAD && segment.p_vaddr == 0 &&
           segment.p_flags == (PF_READ | PF_EXEC) && segment.p_filesz == Http2CompatTextSize &&
           segment.p_memsz == Http2CompatTextSize;
}

// The R_X86_64_JUMP_SLOT relocation behind each slot, checked to import the exact NID from
// libSceHttp2 version 1. Throws on any mismatch. Shared by the Android and desktop loaders.
template <class ModuleT>
std::vector<Http2ImportBinding> CollectHttp2ImportBindings(ModuleT& m) {
    std::vector<Http2ImportBinding> bindings;
    for (const auto& expected : Http2CompatBindings) {
        m.ForEachRelocation([&](const elf_relocation* rel, u32, bool jump) {
            if (rel->rel_offset != expected.got)
                return;
            const auto& d = m.dynamic_info;
            if (!jump || rel->GetType() != R_X86_64_JUMP_SLOT || rel->rel_addend != 0 ||
                rel->GetSymbol() >= d.symbol_table_total_size / sizeof(elf_symbol))
                throw std::runtime_error("HTTP2 content compatibility relocation mismatch");
            const auto& sym = d.symbol_table[rel->GetSymbol()];
            if (sym.st_name >= d.str_table_size || sym.GetType() != STT_FUN || sym.st_shndx != 0)
                throw std::runtime_error("HTTP2 content compatibility symbol mismatch");
            const char* name = d.str_table + sym.st_name;
            const auto* end =
                static_cast<const char*>(std::memchr(name, 0, d.str_table_size - sym.st_name));
            if (!end)
                throw std::runtime_error("HTTP2 content compatibility unterminated import");
            const std::string_view full{name, size_t(end - name)};
            const auto first = full.find('#');
            const auto second = full.find('#', first == full.npos ? full.size() : first + 1);
            if (first == full.npos || second == full.npos || full.substr(0, first) != expected.nid)
                throw std::runtime_error("HTTP2 content compatibility NID mismatch");
            const auto* library = m.FindLibrary(full.substr(first + 1, second - first - 1));
            const auto* module = m.FindModule(full.substr(second + 1));
            if (!library || !module || library->name != "libSceHttp2" || library->version != 1 ||
                module->name != "libSceHttp2")
                throw std::runtime_error("HTTP2 content compatibility provider mismatch");
            bindings.push_back(expected);
        });
    }
    return bindings;
}
} // namespace Core::HostRuntime
