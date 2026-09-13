// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vector>
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"

namespace Core::GuestCpu::Fex {

// The pinned FEX frontend can fold a conditional jump to the current guest
// entry into an IR edge even with MULTIBLOCK=0. The ARM64 target of that edge is
// after EmitEntryPoint, so a warmed self-loop never polls InterruptFaultPage.
// Route guest-entry edges through an ordinary ExitFunction instead. Its linked
// target includes the entry poll. Instruction-internal loops (REP, atomics, etc.)
// have no guest EntryPoint and must retain their original semantics.
//
// Inserted after FEX's register allocator: these edge blocks use only an inline
// RIP constant and ExitFunction, which allocate no virtual/physical registers.
// This private pass interface is tied to the audited FEX gitlink.
class EntryBackedgePass final : public FEXCore::IR::Pass {
public:
    void Run(FEXCore::IR::IREmitter* emitter) override {
        namespace IR = FEXCore::IR;
        auto ir = emitter->ViewIR();
        struct Edge {
            IR::Ref source;
            bool conditional;
            bool true_edge;
            std::uint32_t offset;
        };
        std::vector<Edge> edges;
        for (auto [block, header] : ir.GetBlocks()) {
            for (auto [node, op] : ir.GetCode(block)) {
                auto add = [&](IR::OrderedNodeWrapper target, bool conditional, bool true_edge) {
                    auto* destination = ir.GetOp<IR::IROp_CodeBlock>(target);
                    if (destination->EntryPoint)
                        edges.push_back(
                            {node, conditional, true_edge, destination->GuestEntryOffset});
                };
                if (op->Op == IR::OP_CONDJUMP) {
                    const auto* jump = op->C<IR::IROp_CondJump>();
                    add(jump->TrueBlock, true, true);
                    add(jump->FalseBlock, true, false);
                } else if (op->Op == IR::OP_JUMP) {
                    add(op->C<IR::IROp_Jump>()->TargetBlock, false, true);
                }
            }
        }
        for (const auto& edge : edges) {
            const auto exit = emitter->CreateNewCodeBlockAtEnd();
            emitter->SetCurrentCodeBlock(exit.Node);
            const auto rip = emitter->_InlineEntrypointOffset(IR::OpSize::i64Bit, edge.offset);
            emitter->_ExitFunction(IR::OpSize::i64Bit, rip.Node, IR::BranchHint::None,
                                   emitter->Invalid(), emitter->Invalid());
            if (edge.conditional) {
                auto* jump = ir.GetOp<IR::IROp_CondJump>(edge.source);
                if (edge.true_edge)
                    emitter->SetTrueJumpTarget(jump, exit.Node);
                else
                    emitter->SetFalseJumpTarget(jump, exit.Node);
            } else {
                emitter->SetJumpTarget(ir.GetOp<IR::IROp_Jump>(edge.source), exit.Node);
            }
        }
    }
};
} // namespace Core::GuestCpu::Fex
