// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include "core/loader/plt_import.h"

using namespace Core::Loader;
int main() {
    unsigned checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #expr); } } while (0)
    // Real 2.04 sem-init entry and both neighbours. The direct target is outside
    // the module; its original JUMP_SLOT remains available at 0x428a1e0.
    std::array<std::uint8_t, 48> original{};
    original.fill(0xcc);
    const std::array<std::uint8_t, 6> left{0xff, 0x25, 0xba, 0x8f, 0x28, 0x04};
    const std::array<std::uint8_t, 6> bad{0xe9, 0xb3, 0xbc, 0x88, 0x81, 0x90};
    const std::array<std::uint8_t, 6> right{0xff, 0x25, 0xaa, 0x8f, 0x28, 0x04};
    std::copy(left.begin(), left.end(), original.begin());
    std::copy(bad.begin(), bad.end(), original.begin() + 16);
    std::copy(right.begin(), right.end(), original.begin() + 32);
    const std::array<std::uint64_t, 3> slots{0x428a1d8, 0x428a1e0, 0x428a1e8};
    const auto find = [&](std::span<const std::uint8_t> code) {
        return FindPreboundPltImports(code, 0x1218, slots);
    };
    const auto repair = find(original);
    CHECK(repair.size() == 1);
    if (repair.size() == 1) {
        CHECK(repair[0].offset == 0x1228);
        CHECK(repair[0].got == 0x428a1e0);
        CHECK(0x1027e4000LL + repair[0].previous_target == 0x84070ee0);
        const std::array<std::uint8_t, 6> expected{0xff, 0x25, 0xb2, 0x8f, 0x28, 0x04};
        CHECK(repair[0].bytes == expected);
        auto fixed = original;
        std::copy(repair[0].bytes.begin(), repair[0].bytes.end(), fixed.begin() + 16);
        CHECK(find(fixed).empty());
    }
    for (std::size_t size = 0; size < original.size(); ++size)
        CHECK(find(std::span{original}.first(size)).empty());
    for (std::size_t i = 0; i < original.size(); ++i) {
        // Displacements may vary; every other byte is part of the required shape.
        if ((i >= 2 && i < 6) || (i >= 17 && i < 21) || (i >= 34 && i < 38)) continue;
        auto damaged = original;
        damaged[i] ^= 1;
        CHECK(find(damaged).empty());
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        std::vector<std::uint64_t> missing(slots.begin(), slots.end());
        missing.erase(missing.begin() + i);
        CHECK(FindPreboundPltImports(original, 0x1218, missing).empty());
    }
    auto local = original;
    std::fill(local.begin() + 17, local.begin() + 21, 0);
    CHECK(find(local).size() == 1); // Local prebinding must use the same resolved ABI too.
    // Adjacent prebindings (the main executable's sem-init/timedwait imports).
    std::array<std::uint8_t, 64> adjacent{};
    adjacent.fill(0xcc);
    std::copy(original.begin(), original.begin() + 32, adjacent.begin());
    std::copy(bad.begin(), bad.end(), adjacent.begin() + 32);
    std::copy(right.begin(), right.end(), adjacent.begin() + 48);
    adjacent[50] -= 8; // PC advanced 16, GOT advanced 8.
    const std::array<std::uint64_t, 4> four_slots{
        0x428a1d8, 0x428a1e0, 0x428a1e8, 0x428a1f0};
    const auto run = FindPreboundPltImports(adjacent, 0x1218, four_slots);
    CHECK(run.size() == 2);
    if (run.size() == 2) {
        CHECK(run[0].got == four_slots[1]);
        CHECK(run[1].got == four_slots[2]);
        CHECK(run[1].offset == 0x1238);
    }
    for (std::size_t size = 0; size < adjacent.size(); ++size)
        CHECK(FindPreboundPltImports(std::span{adjacent}.first(size), 0x1218, four_slots).empty());
    for (std::size_t i = 0; i < four_slots.size(); ++i) {
        std::vector<std::uint64_t> missing(four_slots.begin(), four_slots.end());
        missing.erase(missing.begin() + i);
        CHECK(FindPreboundPltImports(adjacent, 0x1218, missing).empty());
    }
    adjacent[37] = 0xcc; // An arbitrary branch is not a prebound import stub.
    CHECK(FindPreboundPltImports(adjacent, 0x1218, four_slots).empty());
    auto mismatched = original;
    ++mismatched[34];
    CHECK(find(mismatched).empty());
    CHECK(FindPreboundPltImports(original, UINT64_MAX, slots).empty());
    CHECK(FindPreboundPltImports(original, INT64_MAX - 40, slots).empty());
    std::printf("PLT_IMPORT_TESTS checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
