// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bit>
#include <string_view>
#include <vector>
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"

namespace Core::GuestCpu::Fex {
// Used only by uncached TF translations while software watchpoints are armed.
// A preflight compile records unsupported memory IR before executing anything.
// Normal translations take one false branch and contain no observation calls.
struct MemoryWatchCompileState {
    bool enabled{}, unsupported{};
    std::uint64_t inserted{};
};
class MemoryWatchPass final : public FEXCore::IR::Pass {
public:
    explicit MemoryWatchPass(MemoryWatchCompileState& state) : state(state) {}
    void Run(FEXCore::IR::IREmitter* e) override {
        if (!state.enabled)
            return;
        namespace IR = FEXCore::IR;
        auto ir = e->ViewIR();
        struct Access {
            IR::Ref node;
            IR::OrderedNodeWrapper address;
            std::uint64_t bytes, mask;
            bool push{};
            IR::OrderedNodeWrapper offset{IR::OrderedNodeWrapper{}};
            IR::MemOffsetType type{IR::MemOffsetType::SXTX};
            std::uint8_t scale{1};
        };
        std::vector<Access> accesses;
        for (auto [block, header] : ir.GetBlocks())
            for (auto [node, op] : ir.GetCode(block)) {
                const auto bytes = IR::OpSizeToSize(op->Size);
                auto simple = [&](auto* x, std::uint64_t mask, std::uint64_t length) {
                    accesses.push_back({node, x->Addr, length, mask});
                };
                auto offset = [&](auto* x, std::uint64_t mask) {
                    simple(x, mask, bytes);
                    accesses.back().offset = x->Offset;
                    accesses.back().type = x->OffsetType;
                    accesses.back().scale = x->OffsetScale;
                    if (!x->Offset.IsInvalid() &&
                        ((x->OffsetScale != 1 && x->OffsetScale != bytes) ||
                         (x->OffsetType != IR::MemOffsetType::SXTX &&
                          x->OffsetType != IR::MemOffsetType::SXTW &&
                          x->OffsetType != IR::MemOffsetType::UXTW)))
                        state.unsupported = true;
                };
                switch (op->Op) {
                case IR::OP_LOADMEM:
                    offset(op->C<IR::IROp_LoadMem>(), 1);
                    break;
                case IR::OP_LOADMEMTSO:
                    offset(op->C<IR::IROp_LoadMemTSO>(), 1);
                    break;
                case IR::OP_STOREMEM:
                    offset(op->C<IR::IROp_StoreMem>(), 2);
                    break;
                case IR::OP_STOREMEMTSO:
                    offset(op->C<IR::IROp_StoreMemTSO>(), 2);
                    break;
                case IR::OP_VLOADVECTORELEMENT:
                    simple(op->C<IR::IROp_VLoadVectorElement>(), 1,
                           IR::OpSizeToSize(op->ElementSize));
                    break;
                case IR::OP_VSTOREVECTORELEMENT:
                    simple(op->C<IR::IROp_VStoreVectorElement>(), 2,
                           IR::OpSizeToSize(op->ElementSize));
                    break;
                case IR::OP_VBROADCASTFROMMEM:
                    accesses.push_back({node, op->C<IR::IROp_VBroadcastFromMem>()->Address,
                                        IR::OpSizeToSize(op->ElementSize), 1});
                    break;
                case IR::OP_PUSH: {
                    const auto* p = op->C<IR::IROp_Push>();
                    accesses.push_back({node, p->Addr, IR::OpSizeToSize(p->ValueSize), 2, true});
                    break;
                }
                case IR::OP_POP:
                    accesses.push_back({node, op->C<IR::IROp_Pop>()->InoutAddr, bytes, 1});
                    break;
#define OBSERVE_ATOMIC(name, kind)                                                                 \
    case IR::OP_##name:                                                                            \
        simple(op->C<IR::IROp_##kind>(), 3, bytes);                                                \
        break
                    OBSERVE_ATOMIC(CAS, CAS);
                    OBSERVE_ATOMIC(ATOMICSWAP, AtomicSwap);
                    OBSERVE_ATOMIC(ATOMICFETCHADD, AtomicFetchAdd);
                    OBSERVE_ATOMIC(ATOMICFETCHSUB, AtomicFetchSub);
                    OBSERVE_ATOMIC(ATOMICFETCHAND, AtomicFetchAnd);
                    OBSERVE_ATOMIC(ATOMICFETCHCLR, AtomicFetchCLR);
                    OBSERVE_ATOMIC(ATOMICFETCHOR, AtomicFetchOr);
                    OBSERVE_ATOMIC(ATOMICFETCHXOR, AtomicFetchXor);
                    OBSERVE_ATOMIC(ATOMICFETCHNEG, AtomicFetchNeg);
#undef OBSERVE_ATOMIC
                case IR::OP_CASPAIR:
                    simple(op->C<IR::IROp_CASPair>(), 3, bytes * 2);
                    break;
                default: {
                    const std::string_view name{IR::GetName(op->Op)};
                    // Remaining guest memory primitives (masked/gather, bulk and
                    // post-RA pairs) are deliberately refused, never silently skipped.
                    if (name.find("Mem") != name.npos || name.starts_with("VLoadVector") ||
                        name.starts_with("VStoreVector") || name.starts_with("VLoadNonTemporal") ||
                        name.starts_with("VStoreNonTemporal") || name.starts_with("Atomic") ||
                        name.starts_with("CAS") || name == "PushTwo" || name == "PopTwo" ||
                        name == "CacheLineZero")
                        state.unsupported = true;
                    break;
                }
                }
            }
        if (state.unsupported)
            return;
        const auto saved = e->GetWriteCursor();
        for (auto& access : accesses) {
            e->SetWriteCursorBefore(access.node);
            IR::Ref address = e->UnwrapNode(access.address);
            if (!access.offset.IsInvalid()) {
                auto offset = e->UnwrapNode(access.offset);
                if (e->IsValueInlineConstant(access.offset))
                    offset = e->Constant(
                        e->GetOpHeader(access.offset)->C<IR::IROp_InlineConstant>()->Constant);
                else {
                    if (access.type == IR::MemOffsetType::UXTW)
                        offset = e->_Bfe(IR::OpSize::i64Bit, 32, 0, offset).Node;
                    if (access.type == IR::MemOffsetType::SXTW)
                        offset = e->_Sbfe(IR::OpSize::i64Bit, 32, 0, offset).Node;
                    if (access.scale != 1)
                        offset = e->_Lshl(IR::OpSize::i64Bit, offset,
                                          e->Constant(std::countr_zero(access.scale)))
                                     .Node;
                }
                address = e->_Add(IR::OpSize::i64Bit, address, offset).Node;
            }
            if (access.push)
                address = e->_Sub(IR::OpSize::i64Bit, address, e->Constant(access.bytes)).Node;
            e->_DebugMemoryAccess(address, access.bytes, access.mask);
            ++state.inserted;
        }
        e->SetWriteCursor(saved);
    }

private:
    MemoryWatchCompileState& state;
};
} // namespace Core::GuestCpu::Fex
