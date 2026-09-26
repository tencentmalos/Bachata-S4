// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Guest GPU command trace (docs/specs/android-graphics-debugging-toolkit.md §4.2-§4.3).
//
// One bounded capture transaction records three layers, ordered by one sequence number:
//   * PM4:     submit envelopes, every indirect buffer entered (DCB, CE, ACB, nested IB)
//              and every consumed packet with its original dwords, as consumed;
//   * actions: each draw/dispatch the Rasterizer decodes, with its shaders, targets,
//              sampled/storage images and buffers read from the resolved sharps;
//   * host:    what the emulator did about it (render pass begin/end and why, buffer
//              barriers, HLE copies and whether they moved before the pass, native
//              resolution decisions, replaced dispatches).
// On completion a worker writes `<uuid>.gpu.pm4.trace` (PM4 layer) and
// `<uuid>.gcmdtrace.ps4` (actions + host) under CapturesDir/gpu-trace;
// tools/ps4-gpu-trace decodes them.
//
// Disabled cost: one relaxed atomic load per packet/action. Records are appended only
// while capturing, bounded by frames, bytes and wall time; hitting a bound ends the
// capture as partial and never blocks or changes guest/GPU execution. The only guest
// memory read is the command dwords being consumed and, once per shader per capture,
// the GCN code span the pipeline cache already hashed.

#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

namespace AmdGpu::Pm4Trace {

inline std::atomic<bool> capturing{false};

[[nodiscard]] inline bool Active() noexcept {
    return capturing.load(std::memory_order_relaxed);
}

enum class IbKind : u32 { Graphics = 0, Constant = 1, Compute = 2 };

// Identity of each indirect-buffer walk; always assigned (cheap) so a capture that
// starts mid-buffer still has consistent ids.
u64 NextIb() noexcept;

void NoteSubmit(u32 queue, u64 submission, u64 dcb_va, u32 dcb_dwords, u64 ccb_va,
                u32 ccb_dwords);
void NoteIbBegin(u64 ib, u64 submission, u32 queue, IbKind kind, u64 va, u32 dwords);
void NoteIbEnd(u64 ib);
// `dwords` is the whole packet as consumed (header included), already bounded by the
// caller to the buffer it is walking. `va` is 0 when the packet has no single guest
// address (split across a compute ring wrap).
void NotePacket(u64 ib, u64 submission, u32 queue, u32 dword_offset, u64 va,
                std::span<const u32> dwords);
// Guest flip accepted (EOP-driven flips arrive on the PM4 consumer thread, in stream
// order). Drives the frame bounds of the capture.
void NoteFlip(u64 flip);

// GCN code of a shader used by a traced action, once per hash per capture. Returns false
// when the hash was already recorded (callers skip locating the code).
bool WantShader(u64 hash);
void NoteShader(u64 hash, u64 base, u32 stage, std::span<const u32> code);

// ---- decoded actions ----
enum class ActionKind : u32 {
    Draw = 1,
    DrawIndexed = 2,
    DrawIndirect = 3,
    DrawIndexedIndirect = 4,
    Dispatch = 5,
    DispatchIndirect = 6,
};
struct ActionStage {
    u32 stage; // Shader::SwStage
    u32 reserved;
    u64 hash;
    u64 base;
};
struct ActionTarget {
    u32 slot;  // 0-7 color, 8 depth
    u32 flags; // bit0 clear, bit1 depth read-only, bit2 has stencil
    u64 address;
    u32 width, height, layers, vk_format;
    u32 scale_eighths, reserved;
};
struct ActionImage {
    u32 stage;
    u32 flags; // bit0 written, bit1 atomic, bit2 depth, bit3 array, bit4 requires native
    u64 address;
    u32 width, height, depth, levels;
    u32 data_format, number_format, type, tiling;
};
struct ActionBuffer {
    u32 stage;
    u32 flags; // bit0 written, bit1 formatted
    u64 address;
    u32 size, stride;
};
struct Action {
    ActionKind kind{};
    u32 p0{}, p1{}, p2{}, p3{}; // draw: count, instances, primitive, index size; dispatch: x, y, z
    u64 p4{};                   // draw: index base or indirect address; dispatch: indirect address
    std::vector<ActionStage> stages;
    std::vector<ActionTarget> targets;
    std::vector<ActionImage> images;
    std::vector<ActionBuffer> buffers;
    void Clear() {
        kind = {};
        p0 = p1 = p2 = p3 = 0;
        p4 = 0;
        stages.clear();
        targets.clear();
        images.clear();
        buffers.clear();
    }
};
// Reserves the id host events of this draw/dispatch attach to; call first.
u64 BeginAction();
void NoteAction(const Action& action);

// ---- host consequences ----
enum class HostEvent : u32 {
    PassBegin = 1,  // a=pass b=resumed c=begin cause d=(w<<32|h) text=targets
    PassEnd = 2,    // a=pass b=end cause c=draws d=load pixels text=break detail
    Barrier = 3,    // a=address b=size c=src access d=dst access text=kind/shader
    HleCopy = 4,    // a=src b=dst c=regions d=bytes text=placement
    Native = 5,     // a=address b=(w<<32|h) c=vk format d=reason text=trigger
    Replaced = 6,   // dispatch handled by an emulator HLE path; text=which
    HoistFail = 7,  // text=why an independent op could not move before the pass
    BarrierHoisted = 8, // a=pass b=barriers: placed before the pass instead of ending it
};
void NoteHost(HostEvent kind, u64 a, u64 b, u64 c, u64 d, std::string_view text = {});

// DebugBus: gpu_command_trace / guest_command_trace
//   status | arm [frames] [delay] [max_mib] | save | cancel
struct Identity {
    std::string run_uuid;
    u64 pid{}, generation{};
    std::string driver;
};
std::string Command(const std::vector<std::string>& args, const Identity& identity, u64 now_ns);

} // namespace AmdGpu::Pm4Trace
