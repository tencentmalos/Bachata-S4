// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <map>
#include <optional>
#include <unordered_map>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"

namespace Shader::Optimization {
namespace {
using Addresses = std::array<u32, 64>; // Guest wave size, independent of host subgroup size.

// Evaluate only pure lane/constant integer expressions. Unknown inputs and cycles
// must not be mistaken for private storage. Arithmetic has the guest's u32 wrap.
std::optional<u32> LaneValue(IR::Value v, u32 lane, u32 depth = 0) {
    if (depth > 64)
        return std::nullopt;
    if (v.IsImmediate())
        return v.Type() == IR::Type::U32 ? std::optional{v.U32()} : std::nullopt;
    const auto* i = v.Inst();
    if (!i)
        return std::nullopt;
    using O = IR::Opcode;
    if (i->GetOpcode() == O::LaneId)
        return lane;
    const auto arg = [&](u32 n) { return LaneValue(i->Arg(n), lane, depth + 1); };
    switch (i->GetOpcode()) {
    case O::IAdd32:
    case O::ISub32:
    case O::IMul32:
    case O::ShiftLeftLogical32:
    case O::ShiftRightLogical32:
    case O::BitwiseAnd32:
    case O::BitwiseOr32:
    case O::BitwiseXor32: {
        const auto a = arg(0), b = arg(1);
        if (!a || !b)
            return std::nullopt;
        switch (i->GetOpcode()) {
        case O::IAdd32:
            return *a + *b;
        case O::ISub32:
            return *a - *b;
        case O::IMul32:
            return *a * *b;
        case O::ShiftLeftLogical32:
            return *b < 32 ? std::optional{*a << *b} : std::nullopt;
        case O::ShiftRightLogical32:
            return *b < 32 ? std::optional{*a >> *b} : std::nullopt;
        case O::BitwiseAnd32:
            return *a & *b;
        case O::BitwiseOr32:
            return *a | *b;
        case O::BitwiseXor32:
            return *a ^ *b;
        default:
            return std::nullopt;
        }
    }
    case O::BitFieldUExtract:
    case O::BitFieldSExtract: {
        const auto a = arg(0), offset = arg(1), count = arg(2);
        if (!a || !offset || !count || *offset >= 32 || !*count || *count > 32 - *offset)
            return std::nullopt;
        const u32 mask = ~0U >> (32 - *count);
        const u32 bits = (*a >> *offset) & mask;
        const u32 sign = 1U << (*count - 1);
        return i->GetOpcode() == O::BitFieldSExtract ? (bits ^ sign) - sign : bits;
    }
    default:
        return std::nullopt;
    }
}
} // namespace

bool FragmentLdsPass(IR::Program& program) {
    if (program.info.hw_stage != HwStage::Fragment)
        return true;
    struct Access {
        IR::Inst* inst;
        u32 slot;
    };
    std::vector<Access> accesses;
    std::map<Addresses, u32> slots;
    // One physical DWORD may belong to only one guest lane and one private slot.
    // This also rejects partial lane-dependent aliasing between two accesses.
    std::unordered_map<u32, std::pair<u32, u32>> owners;
    using O = IR::Opcode;
    for (auto* block : program.blocks) {
        for (auto& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            const bool shared = op >= O::LoadSharedU16 && op <= O::WriteSharedU64;
            const bool atomic = op >= O::SharedAtomicIAdd32 && op <= O::SharedAtomicXor64;
            if ((!shared && !atomic) || inst.Flags<bool>()) // GDS remains a storage buffer.
                continue;
            if (op != O::LoadSharedU32 && op != O::WriteSharedU32)
                return false;
            Addresses addresses{};
            for (u32 lane = 0; lane < addresses.size(); ++lane) {
                auto value = LaneValue(inst.Arg(0), lane);
                if (!value || (*value & 3U))
                    return false;
                addresses[lane] = *value;
            }
            const auto [slot, inserted] = slots.try_emplace(addresses, slots.size());
            if (inserted) {
                for (u32 lane = 0; lane < addresses.size(); ++lane) {
                    const auto owner = std::pair{lane, slot->second};
                    if (!owners.try_emplace(addresses[lane], owner).second)
                        return false;
                }
            }
            accesses.push_back({&inst, slot->second});
        }
    }
    if (accesses.empty())
        return true;
    const u32 base = program.next_reg_index;
    program.next_reg_index += slots.size();
    for (const auto& [inst, slot] : accesses) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::VirtualReg reg{base + slot, IR::Type::U32};
        if (inst->GetOpcode() == O::LoadSharedU32) {
            inst->ReplaceUsesWithAndRemove(ir.GetVirtualReg(reg));
        } else {
            ir.SetVirtualReg(reg, inst->Arg(1));
            inst->Invalidate();
        }
    }
    // Reconstruct per-lane values through the existing CFG, including loop/branch
    // merges. No Workgroup storage or fragment-stage barriers are introduced.
    for (auto* block : program.blocks)
        block->ssa_state.Reset();
    SsaRewritePass(program);
    return true;
}
} // namespace Shader::Optimization
