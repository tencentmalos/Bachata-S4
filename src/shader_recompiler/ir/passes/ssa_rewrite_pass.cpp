// SPDX-FileCopyrightText: Copyright 2025 Philip Rebohle
// SPDX-License-Identifier: MIT

// This file implements the SSA rewriting algorithm proposed in
//
//      Simple and Efficient Construction of Static Single Assignment Form.
//      Braun M., Buchwald S., Hack S., Leiba R., Mallon C., Zwinkau A. (2013)
//      In: Jhala R., De Bosschere K. (eds)
//      Compiler Construction. CC 2013.
//      Lecture Notes in Computer Science, vol 7791.
//      Springer, Berlin, Heidelberg
//
//      https://link.springer.com/chapter/10.1007/978-3-642-37051-9_6
//

#include <algorithm>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/value.h"

namespace Shader::Optimization {
namespace {

using RegType = IR::RegType;
using ValueMap = std::unordered_map<IR::Block*, IR::Value>;

struct DefTable {
    const IR::Value& Def(IR::Block* block, IR::RegTag tag) {
        if (tag.IsIntrusive()) {
            return block->ssa_state.values[tag.Index()];
        }
        switch (tag.type) {
        case RegType::GotoVariable:
            return goto_vars[tag.index][block];
        case RegType::VirtualReg:
            return reg_vars[tag.reg.Key()][block];
        default:
            UNREACHABLE();
        }
    }
    void SetDef(IR::Block* block, IR::RegTag tag, const IR::Value& value) {
        if (tag.IsIntrusive()) {
            block->ssa_state.values[tag.Index()] = value;
            return;
        }
        switch (tag.type) {
        case RegType::GotoVariable:
            goto_vars[tag.index].insert_or_assign(block, value);
            return;
        case RegType::VirtualReg:
            reg_vars[tag.reg.Key()].insert_or_assign(block, value);
            return;
        default:
            UNREACHABLE();
        }
    }

    std::unordered_map<u32, ValueMap> goto_vars;
    std::unordered_map<u64, ValueMap> reg_vars;
};

constexpr IR::Type TypeOf(IR::RegTag tag) noexcept {
    switch (tag.type) {
    case RegType::ScalarReg:
    case RegType::VectorReg:
    case RegType::VccLo:
    case RegType::VccHi:
    case RegType::M0:
        return IR::Type::U32;
    case RegType::Scc:
    case RegType::Exec:
    case RegType::GotoVariable:
        return IR::Type::U1;
    case RegType::VirtualReg:
        return tag.reg.type;
    default:
        UNREACHABLE();
    }
}

constexpr IR::Opcode UndefOpcode(IR::RegTag tag) noexcept {
    switch (tag.type) {
    case RegType::ScalarReg:
    case RegType::VectorReg:
    case RegType::VccLo:
    case RegType::VccHi:
    case RegType::M0:
        return IR::Opcode::UndefU32;
    case RegType::Scc:
    case RegType::Exec:
    case RegType::GotoVariable:
        return IR::Opcode::UndefU1;
    case RegType::VirtualReg:
        switch (tag.reg.type) {
        case IR::Type::U64:
            return IR::Opcode::UndefU64;
        case IR::Type::U32:
            return IR::Opcode::UndefU32;
        case IR::Type::U32x2:
            return IR::Opcode::UndefU32x2;
        case IR::Type::U32x3:
            return IR::Opcode::UndefU32x3;
        case IR::Type::U32x4:
            return IR::Opcode::UndefU32x4;
        case IR::Type::F32:
            return IR::Opcode::UndefF32;
        case IR::Type::U1:
            return IR::Opcode::UndefU1;
        default:
            UNREACHABLE_MSG("Unknown virtual reg type {}", tag.reg.type);
        }
    default:
        UNREACHABLE_MSG("Unknown reg type {}", magic_enum::enum_name(tag.type));
    }
}

/// Blocks are filled in reverse post order. A block is sealed once all its predecessors are
/// filled: from then on a phi created in it gets its operands at once and is removed right away
/// when it is trivial, so later reads see the value it stands for. Loop headers are sealed after
/// their back edges; until then reads there create incomplete phis that are completed on sealing.
class Pass {
public:
    explicit Pass(const IR::BlockList& post_order) : blocks(post_order.size()) {
        for (size_t i = 0; i < post_order.size(); ++i) {
            blocks[i].block = post_order[i];
        }
    }

    void WriteVariable(IR::RegTag tag, IR::Block* block, const IR::Value& value) {
        current_def.SetDef(block, tag, value);
    }

    IR::Value ReadVariable(IR::RegTag tag, IR::Block* block) {
        boost::container::small_vector<IR::Block*, 16> chain;
        IR::Value result{};

        while (block) {
            if (const IR::Value& def = current_def.Def(block, tag); !def.IsEmpty()) {
                result = Resolve(def);
                break;
            }

            const auto preds = block->ImmPredecessors();
            const bool sealed = IsSealed(block);
            if (sealed && preds.size() == 1) {
                // Optimize the common case of one predecessor: no phi needed
                chain.push_back(std::exchange(block, preds.front()));
                continue;
            } else if (sealed && preds.empty()) {
                result = IR::Value{&*block->PrependNewInst(block->begin(), UndefOpcode(tag))};
                WriteVariable(tag, block, result);
                break;
            }

            // A join block, or one whose predecessors are not all filled yet. The phi is the
            // variable's definition while its operands are read, which breaks cycles.
            IR::Inst* const phi{&*block->PrependNewInst(block->begin(), IR::Opcode::Phi)};
            phi->SetFlags(TypeOf(tag));
            phi->SetRegTag(tag);
            all_phis.push_back(phi);
            result = IR::Value{phi};
            WriteVariable(tag, block, result);
            if (!sealed) {
                incomplete.push_back(phi);
            } else if (depth >= MaxDepth) {
                // Deep chain of joins: finish it from the top level instead of the stack.
                deferred.push_back(phi);
            } else {
                result = AddPhiOperands(phi);
                if (result != IR::Value{phi}) {
                    WriteVariable(tag, block, result);
                }
            }
            break;
        }

        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            WriteVariable(tag, *it, result);
        }
        if (depth == 0) {
            DrainDeferred();
            result = Resolve(result);
        }
        return result;
    }

    /// Before a block is filled.
    void Enter(IR::Block* block) {
        if (!IsSealed(block) && PredecessorsFilled(block)) {
            Seal(block);
        }
    }

    /// After a block is filled: seals the successors whose predecessors are now all filled.
    void Leave(IR::Block* block) {
        if (auto* state = State(block)) {
            state->filled = true;
        }
        for (IR::Block* succ : block->ImmSuccessors()) {
            if (!IsSealed(succ) && PredecessorsFilled(succ)) {
                Seal(succ);
            }
        }
    }

    /// After every block is filled: completes what is left (phis in blocks that a block
    /// outside the post order leads to) and removes any trivial phi still in place.
    void Finish() {
        all_sealed = true;
        while (!incomplete.empty()) {
            IR::Inst* const phi = incomplete.back();
            incomplete.pop_back();
            AddPhiOperands(phi);
            DrainDeferred();
        }
        ResolveTrivialPhis();
        for (IR::Inst* phi : all_phis) {
            phi->SetDefinition<u32>(0);
        }
    }

private:
    static constexpr u32 MaxDepth = 64;

    struct BlockState {
        bool filled{};
        bool sealed{};
    };

    /// Blocks in the post order; others (not reachable from the entry) have no state and count
    /// as never filled.
    BlockState* State(IR::Block* block) {
        const u32 index = block->po_index;
        if (index < blocks.size() && post_order_block(index) == block) {
            return &blocks[index].state;
        }
        return nullptr;
    }
    IR::Block* post_order_block(u32 index) const {
        return blocks[index].block;
    }

    bool IsSealed(IR::Block* block) {
        if (all_sealed) {
            return true;
        }
        const auto* state = State(block);
        return state && state->sealed;
    }

    bool PredecessorsFilled(IR::Block* block) {
        return std::ranges::all_of(block->ImmPredecessors(), [this](IR::Block* pred) {
            const auto* state = State(pred);
            return state && state->filled;
        });
    }

    void Seal(IR::Block* block) {
        State(block)->sealed = true;
        // Complete the block's incomplete phis; other blocks' stay queued.
        const auto first = std::stable_partition(incomplete.begin(), incomplete.end(),
                                                 [block](IR::Inst* phi) {
                                                     return phi->GetParent() != block;
                                                 });
        boost::container::small_vector<IR::Inst*, 16> phis(first, incomplete.end());
        incomplete.erase(first, incomplete.end());
        for (IR::Inst* phi : phis) {
            AddPhiOperands(phi);
            DrainDeferred();
        }
    }

    /// Reads the phi's operands from its block's predecessors, then removes it if trivial.
    /// Returns what the phi stands for.
    IR::Value AddPhiOperands(IR::Inst* phi) {
        building.push_back(phi);
        const IR::RegTag tag = phi->GetRegTag();
        for (IR::Block* pred : phi->GetParent()->ImmPredecessors()) {
            phi->AddPhiOperand(pred, ReadVariableNested(tag, pred));
        }
        building.pop_back();
        return TryRemoveTrivialPhi(phi);
    }

    /// ReadVariable from inside a phi under construction: deferred phis are finished by the
    /// outermost read.
    IR::Value ReadVariableNested(IR::RegTag tag, IR::Block* block) {
        ++depth;
        IR::Value value = ReadVariable(tag, block);
        --depth;
        return Resolve(value);
    }

    /// A phi whose operands are all the same value (or itself) is replaced by that value; phis
    /// that used it may have become trivial in turn. Returns the value the phi stands for.
    IR::Value TryRemoveTrivialPhi(IR::Inst* phi) {
        IR::Value same;
        for (size_t i = 0; i < phi->NumArgs(); ++i) {
            const IR::Value op{phi->Arg(i)};
            if (op == same || op == IR::Value{phi}) {
                // Unique value or self-reference
                continue;
            }
            if (!same.IsEmpty()) {
                // The phi merges at least two values: not trivial
                return IR::Value{phi};
            }
            same = op;
        }

        IR::Block* block = phi->GetParent();
        if (same.IsEmpty()) {
            // All operands are self-references or phi has no operands
            auto& list = block->Instructions();
            auto reinsert_point = std::ranges::find_if_not(list, IR::IsPhi);
            same = IR::Value{
                &*block->PrependNewInst(reinsert_point, UndefOpcode(phi->GetRegTag()))};
        }

        boost::container::small_vector<IR::Inst*, 8> phi_users;
        for (const auto& [user, operand] : phi->Uses()) {
            if (user->GetOpcode() == IR::Opcode::Phi && user != phi) {
                phi_users.push_back(user);
            }
        }
        phi->ReplaceUsesWithAndRemove(same);
        block->Instructions().erase(IR::Block::InstructionList::s_iterator_to(*phi));
        // Removed phis keep their memory: the definition slot (unused until SPIR-V emission)
        // points to the replacement, for definitions recorded before the removal.
        forward.push_back(same);
        phi->SetDefinition<u32>(static_cast<u32>(forward.size()));

        for (IR::Inst* user : phi_users) {
            if (user->GetOpcode() != IR::Opcode::Phi || IsBuilding(user)) {
                continue; // Removed already, or checked when its operands are complete.
            }
            if (depth >= MaxDepth) {
                recheck.push_back(user);
                continue;
            }
            ++depth;
            TryRemoveTrivialPhi(user);
            --depth;
        }
        return Resolve(same);
    }

    bool IsBuilding(IR::Inst* phi) const {
        return std::ranges::find(building, phi) != building.end();
    }

    /// The value a removed phi stands for, following later removals.
    IR::Value Resolve(IR::Value value) const {
        while (IR::Inst* inst = value.TryInst()) {
            if (inst->GetOpcode() != IR::Opcode::Void) {
                break;
            }
            const u32 slot = inst->Definition<u32>();
            if (slot == 0) {
                break;
            }
            value = forward[slot - 1];
        }
        return value;
    }

    void DrainDeferred() {
        if (depth != 0) {
            return;
        }
        while (!deferred.empty() || !recheck.empty()) {
            if (!deferred.empty()) {
                IR::Inst* const phi = deferred.back();
                deferred.pop_back();
                const IR::Value value = AddPhiOperands(phi);
                if (value != IR::Value{phi}) {
                    WriteVariable(phi->GetRegTag(), phi->GetParent(), value);
                }
                continue;
            }
            IR::Inst* const phi = recheck.back();
            recheck.pop_back();
            if (phi->GetOpcode() == IR::Opcode::Phi && !IsBuilding(phi)) {
                TryRemoveTrivialPhi(phi);
            }
        }
    }

    /// Safety net: any phi still trivial after everything is complete (one whose removal was
    /// skipped while a user was under construction) goes now, as in a separate pass.
    void ResolveTrivialPhis() {
        std::vector<IR::Inst*> worklist;
        for (IR::Inst* phi : all_phis) {
            if (phi->GetOpcode() == IR::Opcode::Phi) {
                worklist.push_back(phi);
            }
        }
        while (!worklist.empty()) {
            IR::Inst* phi = worklist.back();
            worklist.pop_back();
            if (phi->GetOpcode() != IR::Opcode::Phi) {
                continue;
            }
            IR::Value same;
            bool non_trivial = false;
            for (size_t i = 0; i < phi->NumArgs(); ++i) {
                const IR::Value op{phi->Arg(i)};
                if (op == same || op == IR::Value{phi}) {
                    continue;
                }
                if (!same.IsEmpty()) {
                    non_trivial = true;
                    break;
                }
                same = op;
            }
            if (non_trivial) {
                continue;
            }
            IR::Block* block = phi->GetParent();
            if (same.IsEmpty()) {
                auto& list = block->Instructions();
                auto reinsert_point = std::ranges::find_if_not(list, IR::IsPhi);
                same = IR::Value{
                    &*block->PrependNewInst(reinsert_point, UndefOpcode(phi->GetRegTag()))};
            }
            for (const auto& [user, operand] : phi->Uses()) {
                if (user->GetOpcode() == IR::Opcode::Phi && user != phi) {
                    worklist.push_back(user);
                }
            }
            phi->ReplaceUsesWithAndRemove(same);
            block->Instructions().erase(IR::Block::InstructionList::s_iterator_to(*phi));
        }
    }

    struct BlockSlot {
        IR::Block* block{};
        BlockState state{};
    };

    DefTable current_def;
    std::vector<BlockSlot> blocks;
    std::vector<IR::Inst*> all_phis;
    std::vector<IR::Inst*> incomplete; ///< Phis of unsealed blocks, waiting for their operands.
    std::vector<IR::Inst*> deferred;   ///< Phis of sealed blocks left for the top level.
    std::vector<IR::Inst*> recheck;    ///< Phis to check for triviality at the top level.
    std::vector<IR::Inst*> building;   ///< Phis whose operands are being read.
    std::vector<IR::Value> forward; ///< Replacements of removed phis, see TryRemoveTrivialPhi.
    u32 depth{};
    bool all_sealed{};
};

void VisitInst(Pass& pass, IR::Block* block, IR::Inst& inst) {
    const IR::Opcode opcode{inst.GetOpcode()};
    switch (opcode) {
    case IR::Opcode::SetScalarRegister:
        pass.WriteVariable(IR::RegTag{inst.Arg(0).ScalarReg()}, block, inst.Arg(1));
        break;
    case IR::Opcode::SetVectorRegister:
        pass.WriteVariable(IR::RegTag{inst.Arg(0).VectorReg()}, block, inst.Arg(1));
        break;
    case IR::Opcode::SetVirtualRegister:
        pass.WriteVariable(IR::RegTag{inst.Arg(0).VirtualReg()}, block, inst.Arg(1));
        break;
    case IR::Opcode::SetGotoVariable:
        pass.WriteVariable(IR::RegTag{RegType::GotoVariable, inst.Arg(0).U32()}, block,
                           inst.Arg(1));
        break;
    case IR::Opcode::SetExec:
        pass.WriteVariable(IR::RegTag{RegType::Exec}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetScc:
        pass.WriteVariable(IR::RegTag{RegType::Scc}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetVccLo:
        pass.WriteVariable(IR::RegTag{RegType::VccLo}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetVccHi:
        pass.WriteVariable(IR::RegTag{RegType::VccHi}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetM0:
        pass.WriteVariable(IR::RegTag{RegType::M0}, block, inst.Arg(0));
        break;
    case IR::Opcode::GetScalarRegister:
        inst.ReplaceUsesWithAndRemove(
            pass.ReadVariable(IR::RegTag{inst.Arg(0).ScalarReg()}, block));
        break;
    case IR::Opcode::GetVectorRegister:
        inst.ReplaceUsesWithAndRemove(
            pass.ReadVariable(IR::RegTag{inst.Arg(0).VectorReg()}, block));
        break;
    case IR::Opcode::GetVirtualRegister:
        inst.ReplaceUsesWithAndRemove(
            pass.ReadVariable(IR::RegTag{inst.Arg(0).VirtualReg()}, block));
        break;
    case IR::Opcode::GetGotoVariable:
        inst.ReplaceUsesWithAndRemove(
            pass.ReadVariable(IR::RegTag{RegType::GotoVariable, inst.Arg(0).U32()}, block));
        break;
    case IR::Opcode::GetExec:
        inst.ReplaceUsesWithAndRemove(pass.ReadVariable(IR::RegTag{RegType::Exec}, block));
        break;
    case IR::Opcode::GetScc:
        inst.ReplaceUsesWithAndRemove(pass.ReadVariable(IR::RegTag{RegType::Scc}, block));
        break;
    case IR::Opcode::GetVccLo:
        inst.ReplaceUsesWithAndRemove(pass.ReadVariable(IR::RegTag{RegType::VccLo}, block));
        break;
    case IR::Opcode::GetVccHi:
        inst.ReplaceUsesWithAndRemove(pass.ReadVariable(IR::RegTag{RegType::VccHi}, block));
        break;
    case IR::Opcode::GetM0:
        inst.ReplaceUsesWithAndRemove(pass.ReadVariable(IR::RegTag{RegType::M0}, block));
        break;
    default:
        break;
    }
}

} // Anonymous namespace

void SsaRewritePass(IR::Program& program) {
    Pass pass{program.post_order_blocks};
    const auto end = program.post_order_blocks.rend();
    for (auto it = program.post_order_blocks.rbegin(); it != end; ++it) {
        IR::Block* block{*it};
        pass.Enter(block);
        for (IR::Inst& inst : block->Instructions()) {
            VisitInst(pass, block, inst);
        }
        pass.Leave(block);
    }
    pass.Finish();
}

} // namespace Shader::Optimization
