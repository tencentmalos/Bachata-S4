// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Core::Loader {

struct PltImportRepair {
    std::uint64_t offset;
    std::uint64_t got;
    std::int64_t previous_target;
    std::array<std::uint8_t, 6> bytes;
};

// Restore prebound entries to the targets selected by normal symbol resolution.
// Two intact indirect entries bracket a run of direct jumps; their GOT stride
// and every intervening undefined-function JUMP_SLOT must agree. This also
// restores local wrappers: mixing their inline objects with HLE operations can
// violate the imported API ABI (for example, sem_t versus a host-owned handle).
inline std::vector<PltImportRepair> FindPreboundPltImports(
    std::span<const std::uint8_t> code, std::uint64_t address,
    std::span<const std::uint64_t> sorted_import_slots) {
    std::vector<PltImportRepair> result;
    constexpr auto max_address = std::uint64_t{std::numeric_limits<std::int64_t>::max()} -
                                 std::numeric_limits<std::int32_t>::max();
    if (code.size() < 48 || address > max_address || code.size() > max_address - address)
        return result;
    const auto rel32 = [](const std::uint8_t* p) {
        return std::bit_cast<std::int32_t>(std::uint32_t{p[0]} |
            (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
            (std::uint32_t{p[3]} << 24));
    };
    const auto padding = [&](std::size_t offset) {
        return std::all_of(code.begin() + offset + 6, code.begin() + offset + 16,
                           [](std::uint8_t b) { return b == 0xcc; });
    };
    const auto is_import = [&](std::int64_t slot) {
        return slot >= 0 && std::binary_search(sorted_import_slots.begin(),
            sorted_import_slots.end(), static_cast<std::uint64_t>(slot));
    };
    auto cursor = code.begin() + 16;
    const auto end = code.end() - 31;
    while (cursor < end) {
        cursor = std::find(cursor, end, 0xe9);
        if (cursor == end) break;
        const auto offset = static_cast<std::size_t>(cursor++ - code.begin());
        if (code[offset - 16] != 0xff || code[offset - 15] != 0x25 ||
            !padding(offset - 16))
            continue;
        auto after = offset;
        while (after <= code.size() - 16 && code[after] == 0xe9 &&
               code[after + 5] == 0x90 && padding(after))
            after += 16;
        if (after == offset || after > code.size() - 16 ||
            code[after] != 0xff || code[after + 1] != 0x25 || !padding(after))
            continue;
        const auto count = (after - offset) / 16;
        const auto pc = static_cast<std::int64_t>(address + offset);
        const auto left = pc - 16 + 6 + rel32(code.data() + offset - 14);
        const auto right = static_cast<std::int64_t>(address + after) + 6 +
                           rel32(code.data() + after + 2);
        // Derive the same eight-byte GOT stride independently from both anchors.
        if (right < left || std::uint64_t(right - left) != (count + 1) * 8)
            continue;
        bool valid = true;
        for (std::size_t i = 0; i <= count + 1; ++i) {
            if (!is_import(left + static_cast<std::int64_t>(i * 8))) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        const auto saved = result.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto entry = offset + i * 16;
            const auto entry_pc = static_cast<std::int64_t>(address + entry);
            const auto got = left + static_cast<std::int64_t>((i + 1) * 8);
            const auto displacement = got - (entry_pc + 6);
            if (displacement < std::numeric_limits<std::int32_t>::min() ||
                displacement > std::numeric_limits<std::int32_t>::max()) {
                result.resize(saved);
                break;
            }
            const auto value = static_cast<std::uint32_t>(displacement);
            result.push_back({static_cast<std::uint64_t>(entry_pc),
                              static_cast<std::uint64_t>(got),
                              entry_pc + 5 + rel32(code.data() + entry + 1),
                              {0xff, 0x25, std::uint8_t(value), std::uint8_t(value >> 8),
                               std::uint8_t(value >> 16), std::uint8_t(value >> 24)}});
        }
        cursor = code.begin() + after;
    }
    return result;
}

} // namespace Core::Loader
