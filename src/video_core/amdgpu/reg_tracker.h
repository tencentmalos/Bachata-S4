// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Register dirty tracking for the command processor (GCN v2 stage C, spec §3.2). Register
// packets are written through RegTracker::Write, which compares each dword with the register
// file: a group's generation changes only when one of its registers takes a different value, so
// state the guest re-sends unchanged with every draw does not count as a change. Consumers keep
// the generation they last saw and skip work while it is unchanged.
//
// A register belongs to at most one group. Registers outside every group are written without
// effect on any generation. Writes that bypass Write (the field assignments of draw packets:
// index base, counts, draw initiator) are not in any group.

#pragma once

#include <array>
#include <bit>
#include <cstring>

#include "common/types.h"
#include "video_core/amdgpu/regs.h"

namespace AmdGpu {

enum class RegGroup : u8 {
    None = 0,
    // Everything Rasterizer::PrepareRenderState and BeginRendering read from registers: color and
    // depth buffers with their clear values, masks, color and depth control, depth render
    // control/override, HTILE base, and the scissors used when a pass has no attachments.
    Targets,
    // The rest of what PipelineCache::RefreshGraphicsKey reads outside the shader stages:
    // clipper, polygon, primitive type, stage enables, LS/HS config, export formats, blend
    // controls, viewports and their depth ranges (depth range emulation), GS mode, stream-out.
    KeyState,
    Count,
};

class RegTracker {
public:
    RegTracker() {
        static const Regs layout{};
        const auto mark = [&](const auto& field, RegGroup group) {
            const auto offset = reinterpret_cast<const u8*>(&field) -
                                reinterpret_cast<const u8*>(layout.reg_array.data());
            const u32 first = static_cast<u32>(offset / sizeof(u32));
            const u32 words = static_cast<u32>((sizeof(field) + sizeof(u32) - 1) / sizeof(u32));
            for (u32 w = first; w < first + words && w < Regs::NumRegs; ++w) {
                group_of[w] = static_cast<u8>(group);
            }
        };
        mark(layout.color_buffers, RegGroup::Targets);
        mark(layout.color_target_mask, RegGroup::Targets);
        mark(layout.color_shader_mask, RegGroup::Targets);
        mark(layout.color_control, RegGroup::Targets);
        mark(layout.depth_buffer, RegGroup::Targets);
        mark(layout.depth_view, RegGroup::Targets);
        mark(layout.depth_control, RegGroup::Targets);
        mark(layout.depth_htile_data_base, RegGroup::Targets);
        mark(layout.depth_render_control, RegGroup::Targets);
        mark(layout.depth_render_override, RegGroup::Targets);
        mark(layout.depth_clear, RegGroup::Targets);
        mark(layout.stencil_clear, RegGroup::Targets);
        mark(layout.screen_scissor, RegGroup::Targets);
        mark(layout.window_scissor, RegGroup::Targets);
        mark(layout.generic_scissor, RegGroup::Targets);
        mark(layout.window_offset, RegGroup::Targets);
        mark(layout.clipper_control, RegGroup::KeyState);
        mark(layout.polygon_control, RegGroup::KeyState);
        mark(layout.primitive_type, RegGroup::KeyState);
        mark(layout.stage_enable, RegGroup::KeyState);
        mark(layout.ls_hs_config, RegGroup::KeyState);
        mark(layout.color_export_format, RegGroup::KeyState);
        mark(layout.blend_control, RegGroup::KeyState);
        mark(layout.viewport_control, RegGroup::KeyState);
        mark(layout.viewports, RegGroup::KeyState);
        mark(layout.viewport_depths, RegGroup::KeyState);
        mark(layout.vgt_gs_mode, RegGroup::KeyState);
        mark(layout.vgt_strmout_config, RegGroup::KeyState);
    }

    /// Copies `count` dwords into the register file at `word` and moves the generation of every
    /// group with a register whose value changed.
    void Write(Regs& regs, u32 word, const u32* data, u32 count) {
        u32* dst = regs.reg_array.data() + word;
        if (std::memcmp(dst, data, count * sizeof(u32)) == 0) {
            return;
        }
        u32 changed = 0;
        for (u32 i = 0; i < count; ++i) {
            if (dst[i] != data[i]) {
                dst[i] = data[i];
                changed |= 1u << group_of[word + i];
            }
        }
        Bump(changed & ~1u);
    }

    /// The register file was replaced as a whole (ClearState, defaults).
    void InvalidateAll() {
        Bump(((1u << static_cast<u32>(RegGroup::Count)) - 1) & ~1u);
    }

    [[nodiscard]] u64 Generation(RegGroup group) const {
        return generations[static_cast<size_t>(group)];
    }

    [[nodiscard]] RegGroup GroupOf(u32 word) const {
        return static_cast<RegGroup>(group_of[word]);
    }

private:
    void Bump(u32 mask) {
        while (mask) {
            generations[std::countr_zero(mask)] = ++counter;
            mask &= mask - 1;
        }
    }

    std::array<u8, Regs::NumRegs> group_of{};
    std::array<u64, static_cast<size_t>(RegGroup::Count)> generations{};
    u64 counter{1};
};

} // namespace AmdGpu
