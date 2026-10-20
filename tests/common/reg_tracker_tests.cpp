// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Register dirty tracking (video_core/amdgpu/reg_tracker.h): writes go into the register file,
// a group's generation moves only when one of its registers changes value, registers outside
// every group move nothing, and invalidation moves every group. Header-only.
#include <cstdio>
#include <memory>
#include <random>
#include "video_core/amdgpu/reg_tracker.h"

using namespace AmdGpu;

static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
        }                                                                                          \
    } while (0)

namespace {

template <typename T>
u32 WordOf(const Regs& regs, const T& field) {
    return u32((reinterpret_cast<const u8*>(&field) -
                reinterpret_cast<const u8*>(regs.reg_array.data())) /
               sizeof(u32));
}

void TestGroupMap(const Regs& regs, const RegTracker& tracker) {
    // Every register PrepareRenderState and BeginRendering read is in Targets.
    const u32 targets[] = {
        WordOf(regs, regs.color_buffers[0]),
        WordOf(regs, regs.color_buffers[7]) + u32(sizeof(ColorBuffer) / 4) - 1,
        WordOf(regs, regs.color_target_mask),
        WordOf(regs, regs.color_control),
        WordOf(regs, regs.depth_buffer),
        WordOf(regs, regs.depth_view),
        WordOf(regs, regs.depth_control),
        WordOf(regs, regs.depth_htile_data_base),
        WordOf(regs, regs.depth_render_control),
        WordOf(regs, regs.depth_render_override),
        WordOf(regs, regs.depth_clear),
        WordOf(regs, regs.stencil_clear),
        WordOf(regs, regs.screen_scissor),
        WordOf(regs, regs.window_scissor),
        WordOf(regs, regs.generic_scissor),
        WordOf(regs, regs.window_offset),
    };
    for (const u32 word : targets) {
        CHECK(tracker.GroupOf(word) == RegGroup::Targets);
    }
    // The rest of what the pipeline key reads outside the shader stages is in KeyState.
    const u32 key_state[] = {
        WordOf(regs, regs.clipper_control),
        WordOf(regs, regs.polygon_control),
        WordOf(regs, regs.primitive_type),
        WordOf(regs, regs.stage_enable),
        WordOf(regs, regs.ls_hs_config),
        WordOf(regs, regs.color_export_format),
        WordOf(regs, regs.blend_control[0]),
        WordOf(regs, regs.blend_control[7]),
        WordOf(regs, regs.viewport_control),
        WordOf(regs, regs.viewports[0]),
        WordOf(regs, regs.viewports[15]) + u32(sizeof(regs.viewports[15]) / 4) - 1,
        WordOf(regs, regs.viewport_depths[0]),
        WordOf(regs, regs.viewport_depths[15]) + 1,
        WordOf(regs, regs.vgt_gs_mode),
        WordOf(regs, regs.vgt_strmout_config),
    };
    for (const u32 word : key_state) {
        CHECK(tracker.GroupOf(word) == RegGroup::KeyState);
    }
    // Shader programs and user data are in no group.
    CHECK(tracker.GroupOf(WordOf(regs, regs.ps_program)) == RegGroup::None);
    CHECK(tracker.GroupOf(WordOf(regs, regs.vs_program.user_data)) == RegGroup::None);
}

void TestWrites(Regs& regs, RegTracker& tracker) {
    const u32 cb0 = WordOf(regs, regs.color_buffers[0]);
    const u64 g0 = tracker.Generation(RegGroup::Targets);
    const u32 base[2] = {0x1000, 0x2000};
    tracker.Write(regs, cb0, base, 2);
    CHECK(regs.reg_array[cb0] == 0x1000 && regs.reg_array[cb0 + 1] == 0x2000);
    const u64 g1 = tracker.Generation(RegGroup::Targets);
    CHECK(g1 != g0);
    // The same values again: no change.
    tracker.Write(regs, cb0, base, 2);
    CHECK(tracker.Generation(RegGroup::Targets) == g1);
    // A register outside every group: written, no generation moves.
    const u32 ud = WordOf(regs, regs.ps_program.user_data);
    const u32 value = 0xabcd;
    tracker.Write(regs, ud, &value, 1);
    CHECK(regs.reg_array[ud] == 0xabcd);
    CHECK(tracker.Generation(RegGroup::Targets) == g1);
    // One changed dword among unchanged ones.
    const u32 changed[2] = {0x1000, 0x2001};
    tracker.Write(regs, cb0, changed, 2);
    CHECK(tracker.Generation(RegGroup::Targets) != g1);
    const u64 g2 = tracker.Generation(RegGroup::Targets);
    const u64 s2 = tracker.Generation(RegGroup::KeyState);
    tracker.InvalidateAll();
    CHECK(tracker.Generation(RegGroup::Targets) != g2);
    CHECK(tracker.Generation(RegGroup::KeyState) != s2);
    // A KeyState change leaves Targets alone.
    const u64 g3 = tracker.Generation(RegGroup::Targets);
    const u32 prim = WordOf(regs, regs.primitive_type);
    const u32 tri = regs.reg_array[prim] + 1;
    tracker.Write(regs, prim, &tri, 1);
    CHECK(tracker.Generation(RegGroup::KeyState) != tracker.Generation(RegGroup::Targets));
    CHECK(tracker.Generation(RegGroup::Targets) == g3);
}

// Random writes against a reference: the register file always equals the reference, and each
// group's generation changes exactly when one of its registers changed value.
void TestRandom(Regs& regs, RegTracker& tracker) {
    auto reference = std::make_unique<Regs>(regs);
    std::mt19937 rng{42};
    const u32 lo = Regs::ContextRegWordOffset;
    const u32 hi = Regs::ContextRegWordOffset + 0x400;
    for (int step = 0; step < 200000; ++step) {
        const u32 word = lo + rng() % (hi - lo);
        const u32 count = 1 + rng() % 8;
        u32 data[8];
        bool target_change = false;
        bool state_change = false;
        for (u32 i = 0; i < count; ++i) {
            data[i] = (rng() % 4 == 0) ? rng() : reference->reg_array[word + i];
            const bool changed = data[i] != reference->reg_array[word + i];
            target_change |= changed && tracker.GroupOf(word + i) == RegGroup::Targets;
            state_change |= changed && tracker.GroupOf(word + i) == RegGroup::KeyState;
            reference->reg_array[word + i] = data[i];
        }
        const u64 before = tracker.Generation(RegGroup::Targets);
        const u64 state_before = tracker.Generation(RegGroup::KeyState);
        tracker.Write(regs, word, data, count);
        CHECK((tracker.Generation(RegGroup::Targets) != before) == target_change);
        CHECK((tracker.Generation(RegGroup::KeyState) != state_before) == state_change);
    }
    CHECK(std::memcmp(regs.reg_array.data(), reference->reg_array.data(),
                      sizeof(regs.reg_array)) == 0);
}

} // namespace

int main() {
    auto regs = std::make_unique<Regs>();
    RegTracker tracker;
    TestGroupMap(*regs, tracker);
    TestWrites(*regs, tracker);
    TestRandom(*regs, tracker);
    std::printf("reg_tracker_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
