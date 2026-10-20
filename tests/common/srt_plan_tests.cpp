// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Prepared SRT plans (shader_recompiler/ir/passes/srt_portable.h): for random valid programs,
// tables and unreadable holes, running the prepared steps writes the same flat buffer and issues
// the same reads, in the same order, as interpreting the commands. Header-only.
#include <cstdio>
#include <random>
#include <vector>
#include "shader_recompiler/ir/passes/srt_portable.h"

using namespace Shader;
using Op = PortableSrt::Op;
using Kind = PortableSrt::Kind;

static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            if (failures < 20)                                                                     \
                std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                         \
        }                                                                                          \
    } while (0)

namespace {

struct Memory {
    // Guest tables at a few addresses; dwords listed in `holes` cannot be read.
    struct Region {
        u64 base;
        std::vector<u32> words;
    };
    std::vector<Region> regions;
    std::vector<u64> holes;
    u64 max_read = 256;

    bool Read(u64 address, void* data, size_t size) const {
        if (size == 0 || size > max_read)
            return false;
        for (u64 hole : holes)
            if (hole >= address && hole < address + size)
                return false;
        for (const auto& r : regions) {
            const u64 bytes = r.words.size() * 4;
            if (address >= r.base && address - r.base <= bytes && size <= bytes - (address - r.base)) {
                std::memcpy(data, reinterpret_cast<const u8*>(r.words.data()) + (address - r.base),
                            size);
                return true;
            }
        }
        return false;
    }
};

struct Call {
    u64 address;
    size_t size;
    bool operator==(const Call&) const = default;
};

// A random valid plan whose pointers mostly land in `memory`.
PortableSrt RandomPlan(std::mt19937& rng, u32 flat_size, const Memory& memory,
                       std::vector<u32>& user_data) {
    PortableSrt p;
    const auto pick = [&](u32 n) { return u32(rng() % n); };
    const auto add_const = [&](u32 value) {
        p.expressions.push_back({Op::Constant, value});
        return u32(p.expressions.size() - 1);
    };
    // A handful of non-constant expressions: flat slots, arithmetic on earlier ones.
    const auto add_dynamic = [&]() -> u32 {
        const u32 n = u32(p.expressions.size());
        switch (pick(4)) {
        case 0:
            p.expressions.push_back({Op::Flat, pick(flat_size)});
            break;
        case 1:
            if (n == 0)
                return add_const(pick(64));
            p.expressions.push_back({Op(u32(Op::Add) + pick(10)), pick(n), pick(n)});
            break;
        case 2:
            if (n == 0)
                return add_const(pick(64));
            p.expressions.push_back({Op::Not, pick(n)});
            break;
        default:
            if (n == 0)
                return add_const(pick(64));
            p.expressions.push_back({Op::Extract, pick(n), pick(n), pick(n)});
            break;
        }
        return u32(p.expressions.size() - 1);
    };
    // Constant offsets: mostly small, sometimes huge (u32 wrap of offset << 2).
    const auto offset_const = [&]() {
        const u32 kind = pick(16);
        if (kind == 0)
            return add_const(0x40000000u + pick(4));
        if (kind == 1)
            return add_const(0xFFFFFFFFu - pick(4));
        return add_const(pick(48));
    };
    u32 depth = 0;
    u32 destination = 16;
    const u32 commands = 1 + pick(48);
    for (u32 c = 0; c < commands && destination < flat_size; ++c) {
        const u32 action = pick(10);
        if (depth == 0 || (action < 2 && depth < 4)) {
            const u32 e = pick(8) == 0 ? add_dynamic() : add_const(pick(4) * 2);
            p.commands.push_back({Kind::Push, e, 0});
            ++depth;
        } else if (action < 3) {
            p.commands.push_back({Kind::Pop, 0, 0});
            --depth;
        } else if (action < 8) {
            // A run of consecutive constant copies, possibly broken in the middle.
            const u32 length = 1 + pick(70);
            u32 start = pick(40);
            for (u32 i = 0; i < length && destination < flat_size; ++i) {
                if (pick(20) == 0)
                    start = pick(40) - i; // break the offset sequence
                const u32 dst = pick(25) == 0 && destination + 1 < flat_size ? destination + 1
                                                                               : destination;
                p.commands.push_back({Kind::Copy, add_const(start + i), dst});
                destination = dst + 1;
            }
        } else if (action < 9) {
            p.commands.push_back({Kind::Copy, add_dynamic(), destination++});
        } else {
            p.commands.push_back({Kind::Copy, offset_const(), destination++});
        }
    }
    while (depth--)
        p.commands.push_back({Kind::Pop, 0, 0});
    // User data: pointers into the regions (or null / garbage) at even dword offsets.
    user_data.assign(16, 0);
    for (u32 i = 0; i < 16; i += 2) {
        u64 pointer = 0;
        const u32 kind = pick(8);
        if (kind < 6 && !memory.regions.empty()) {
            const auto& r = memory.regions[pick(u32(memory.regions.size()))];
            pointer = r.base + pick(16) * 4;
        } else if (kind == 6) {
            pointer = (u64(rng()) << 32) | rng();
        }
        user_data[i] = u32(pointer);
        user_data[i + 1] = u32(pointer >> 32);
    }
    return p;
}

Memory RandomMemory(std::mt19937& rng) {
    Memory m;
    const u32 regions = 1 + rng() % 4;
    for (u32 r = 0; r < regions; ++r) {
        Memory::Region region;
        region.base = 0x10000ull * (r + 1) + (rng() % 4) * 4;
        region.words.resize(64 + rng() % 64);
        for (auto& w : region.words) {
            // Some words are pointers to other regions (nested tables).
            if (rng() % 4 == 0)
                w = u32(0x10000ull * (1 + rng() % regions) + (rng() % 16) * 4);
            else
                w = rng() % 2 ? rng() : 0;
        }
        m.regions.push_back(std::move(region));
    }
    const u32 holes = rng() % 4;
    for (u32 h = 0; h < holes; ++h) {
        const auto& r = m.regions[rng() % m.regions.size()];
        m.holes.push_back(r.base + (rng() % r.words.size()) * 4);
    }
    m.max_read = rng() % 4 == 0 ? 8 : 256;
    return m;
}

void TestRandom() {
    std::mt19937 rng{1234};
    u32 valid = 0;
    for (int iteration = 0; iteration < 20000; ++iteration) {
        const Memory memory = RandomMemory(rng);
        std::vector<u32> user_data;
        const u32 flat_size = 32 + rng() % 200;
        PortableSrt plan = RandomPlan(rng, flat_size, memory, user_data);
        if (!plan.Validate(flat_size))
            continue;
        ++valid;
        PortableSrt prepared = plan;
        prepared.Prepare();
        CHECK(prepared.prepared && !plan.prepared);

        std::vector<u32> initial(flat_size);
        std::memcpy(initial.data(), user_data.data(), 16 * 4);
        for (u32 i = 16; i < flat_size; ++i)
            initial[i] = rng(); // stale contents from an earlier walk

        std::vector<Call> calls_a, calls_b;
        auto flat_a = initial;
        auto flat_b = initial;
        auto read_a = [&](u64 address, void* data, size_t size) {
            calls_a.push_back({address, size});
            return memory.Read(address, data, size);
        };
        auto read_b = [&](u64 address, void* data, size_t size) {
            calls_b.push_back({address, size});
            return memory.Read(address, data, size);
        };
        plan.RunInterpreted(user_data, flat_a, read_a);
        PortableSrt::run_mode = u32(PortableSrt::RunMode::Plan);
        prepared.Run(user_data, flat_b, read_b);
        CHECK(flat_a == flat_b);
        CHECK(calls_a == calls_b);
        static bool dumped = false;
        if (!dumped && calls_a != calls_b) {
            dumped = true;
            std::fprintf(stderr, "commands:");
            for (auto c : plan.commands)
                std::fprintf(stderr, " %u:%u(%u,a=%u)->%u", u32(c.kind), c.expression,
                             u32(plan.expressions[c.expression].op),
                             plan.expressions[c.expression].a, c.destination);
            std::fprintf(stderr, "\nsteps:");
            for (auto st : prepared.steps)
                std::fprintf(stderr, " %u:%u,%u,%u", u32(st.type), st.value, st.destination, st.count);
            std::fprintf(stderr, "\ninterp(%zu):", calls_a.size());
            for (auto c : calls_a)
                std::fprintf(stderr, " %llx/%zu", (unsigned long long)c.address, c.size);
            std::fprintf(stderr, "\nplan(%zu):", calls_b.size());
            for (auto c : calls_b)
                std::fprintf(stderr, " %llx/%zu", (unsigned long long)c.address, c.size);
            std::fprintf(stderr, "\nmax_read=%llu\n", (unsigned long long)memory.max_read);
        }

        // Run() on an unprepared plan interprets; Verify keeps the interpreter's result.
        std::vector<u32> flat_c = initial;
        plan.Run(user_data, flat_c, [&](u64 a, void* d, size_t s) { return memory.Read(a, d, s); });
        CHECK(flat_c == flat_a);
        PortableSrt::run_mode = u32(PortableSrt::RunMode::Verify);
        const u64 mismatches = PortableSrt::run_stats.mismatches.load();
        std::vector<u32> flat_d = initial;
        prepared.Run(user_data, flat_d, [&](u64 a, void* d, size_t s) { return memory.Read(a, d, s); });
        CHECK(flat_d == flat_a);
        CHECK(PortableSrt::run_stats.mismatches.load() == mismatches);
        PortableSrt::run_mode = u32(PortableSrt::RunMode::Plan);
    }
    CHECK(valid > 15000);
    std::printf("random plans: %u valid\n", valid);
}

// The merge rule: runs stop at 64 dwords, at a destination gap, at an offset gap and at an
// offset that only matches after 32-bit wrap.
void TestRuns() {
    PortableSrt p;
    p.expressions.push_back({Op::Constant, 0});
    p.commands.push_back({Kind::Push, 0, 0});
    for (u32 i = 0; i < 70; ++i) {
        p.expressions.push_back({Op::Constant, i});
        p.commands.push_back({Kind::Copy, u32(p.expressions.size() - 1), 16 + i});
    }
    // offset 0xFFFFFFFC then (wrapping) 0: not consecutive
    p.expressions.push_back({Op::Constant, 0x3FFFFFFFu});
    p.commands.push_back({Kind::Copy, u32(p.expressions.size() - 1), 100});
    p.expressions.push_back({Op::Constant, 0x40000000u});
    p.commands.push_back({Kind::Copy, u32(p.expressions.size() - 1), 101});
    p.commands.push_back({Kind::Pop, 0, 0});
    CHECK(p.Validate(128));
    p.Prepare();
    using T = PortableSrt::Step::Type;
    CHECK(p.steps.size() == 6);
    CHECK(p.steps[0].type == T::PushConstant && p.steps[0].value == 0);
    CHECK(p.steps[1].type == T::CopyRun && p.steps[1].count == 64 && p.steps[1].destination == 16);
    CHECK(p.steps[2].type == T::CopyRun && p.steps[2].count == 6 && p.steps[2].value == 64 * 4);
    CHECK(p.steps[3].type == T::CopyRun && p.steps[3].count == 1 && p.steps[3].value == 0xFFFFFFFCu);
    CHECK(p.steps[4].type == T::CopyRun && p.steps[4].count == 1 && p.steps[4].value == 0);
    CHECK(p.steps[5].type == T::Pop);
    CHECK(!p.step_expressions);
}

} // namespace

int main() {
    TestRuns();
    TestRandom();
    std::printf("srt_plan_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
