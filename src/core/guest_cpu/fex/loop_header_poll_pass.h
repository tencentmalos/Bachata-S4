// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"

namespace Core::GuestCpu::Fex {

// With MULTIBLOCK=1 a FEX region holds several guest blocks, and a jump inside it lands after
// the region's entry preamble. A loop whose header is not the region start therefore never
// reaches the entry poll, and the taken side of a conditional jump has no probe at all, so a
// pause, cancel or debugger stop could wait forever on such a loop.
//
// This pass puts a poll at the start of every guest block that is the target of a backward
// jump inside the region (any cycle in host block order has one). The poll is one store to the
// interrupt page at LoopPollOffset, 8 bytes past the entry probe's address, so the fault
// handler can tell it apart. A GuestOpcode marker at the same host PC records the block's
// guest RIP in the region's RIP table; the handler looks up that exact PC to resume there.
// At a guest block start the guest registers are flushed and the flags are in their canonical
// host places, the same state as at a region entry.
//
// Jumps to the region start are left to EntryBackedgePass. Jumps to blocks FEX creates inside
// one guest instruction (REP, atomics) are left alone: those are not guest boundaries.
//
// Runs after FEX's register allocator: it adds only a GuestOpcode marker, an inline zero and a
// context store, none of which need a register. Tied to the audited FEX gitlink.
class LoopHeaderPollPass final : public FEXCore::IR::Pass {
public:
    explicit LoopHeaderPollPass(std::uint32_t loop_poll_offset) : offset{loop_poll_offset} {}

    void Run(FEXCore::IR::IREmitter* emitter) override {
        namespace IR = FEXCore::IR;
        auto ir = emitter->ViewIR();
        std::unordered_map<const void*, std::size_t> position;
        std::size_t index = 0;
        for (auto [block, header] : ir.GetBlocks())
            position.emplace(block, index++);

        std::vector<IR::Ref> headers;
        auto guest_block = [&](IR::OrderedNodeWrapper target) {
            const auto* destination = ir.GetOp<IR::IROp_CodeBlock>(target);
            // The region start is the only guest block at offset 0.
            return destination->GuestEntryOffset != 0;
        };
        for (auto [block, header] : ir.GetBlocks()) {
            const auto source = position.at(block);
            for (auto [node, op] : ir.GetCode(block)) {
                auto add = [&](IR::OrderedNodeWrapper target) {
                    const auto node = ir.GetNode(target);
                    if (guest_block(target) && position.at(node) <= source &&
                        std::find(headers.begin(), headers.end(), node) == headers.end())
                        headers.push_back(node);
                };
                if (op->Op == IR::OP_CONDJUMP) {
                    const auto* jump = op->C<IR::IROp_CondJump>();
                    add(jump->TrueBlock);
                    add(jump->FalseBlock);
                } else if (op->Op == IR::OP_JUMP) {
                    add(op->C<IR::IROp_Jump>()->TargetBlock);
                }
            }
        }
        for (const auto block : headers) {
            const auto guest_offset = ir.GetOp<IR::IROp_CodeBlock>(block)->GuestEntryOffset;
            emitter->SetCurrentCodeBlock(block);
            emitter->_GuestOpcode(guest_offset);
            emitter->_StoreContext(IR::OpSize::i64Bit, IR::RegClass::GPR,
                                   emitter->_InlineConstant(0).Node, offset);
        }
    }

private:
    std::uint32_t offset;
};

} // namespace Core::GuestCpu::Fex
