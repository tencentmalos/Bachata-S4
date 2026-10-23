// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"
#include "core/guest_cpu/fex/raw_flag_stores.h"

namespace Core::GuestCpu::Fex {
// Enabled for every translation of an owner while software watchpoints are armed. Those are
// cached translations of one guest instruction each (FEX MaxInstPerBlock 1; all code is discarded
// when the first watch is armed and when the last is removed), so a hit, which protects the owner's
// interrupt page, stops it at the next block entry: right after the accessing instruction.
// The observation carries the accessing instruction's RIP. Memory forms the pass cannot observe set
// `unsupported` (the single-step watch mode refuses those instructions before executing them) and
// are left unobserved in cached translations; `skipped` counts them.
// Normal translations take one false branch and contain no observation calls.
struct MemoryWatchCompileState {
    bool enabled{}, unsupported{};
    std::uint64_t inserted{}, skipped{};
};
// Access flag of an observation whose length is only known at run time (REP MOVS/STOS): Bytes
// is the element size and the observer reads the count (RCX) and DF from the spilled guest state.
inline constexpr std::uint64_t kWatchBulkAccess = 0x100;
// The Access operand also carries the guest RIP of the accessing instruction above this shift.
inline constexpr unsigned kWatchRipShift = 16;
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
            std::uint64_t immediate{}; // constant offset of the paired forms
            bool bulk{};
            std::uint64_t rip{};
        };
        std::vector<Access> accesses;
        const auto block_rip = ir.GetHeader()->OriginalRIP;
        std::uint64_t rip = block_rip;
        for (auto [block, header] : ir.GetBlocks())
            for (auto [node, op] : ir.GetCode(block)) {
                if (op->Op == IR::OP_GUESTOPCODE) {
                    rip = block_rip + op->C<IR::IROp_GuestOpcode>()->GuestEntryOffset;
                    continue;
                }
                const auto before = accesses.size();
                const bool was_unsupported = state.unsupported;
                state.unsupported = false;
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
                // FEX's AVX128 mode splits 256-bit YMM loads/stores into these pairs of
                // 128-bit halves at Addr + Offset (XSAVE/FXSAVE images use them too).
                case IR::OP_LOADMEMPAIR: {
                    const auto* x = op->C<IR::IROp_LoadMemPair>();
                    accesses.push_back({node, x->Addr, static_cast<std::uint64_t>(bytes) * 2, 1});
                    accesses.back().immediate = x->Offset;
                    break;
                }
                case IR::OP_STOREMEMPAIR: {
                    const auto* x = op->C<IR::IROp_StoreMemPair>();
                    accesses.push_back({node, x->Addr, static_cast<std::uint64_t>(bytes) * 2, 2});
                    accesses.back().immediate = x->Offset;
                    break;
                }
                // x87 FST/FSTP/FIST to memory, before FEX's x87 pass lowers it (80, 64, 32 or
                // 16 bits; the index scale can be any power of two here).
                case IR::OP_STORESTACKMEM: {
                    const auto* x = op->C<IR::IROp_StoreStackMem>();
                    accesses.push_back({node, x->Addr, IR::OpSizeToSize(x->StoreSize), 2});
                    accesses.back().offset = x->Offset;
                    accesses.back().type = x->OffsetType;
                    accesses.back().scale = x->OffsetScale;
                    if (x->Addr.IsInvalid() ||
                        (!x->Offset.IsInvalid() &&
                         (!std::has_single_bit(static_cast<unsigned>(x->OffsetScale)) ||
                          (x->OffsetType != IR::MemOffsetType::SXTX &&
                           x->OffsetType != IR::MemOffsetType::SXTW &&
                           x->OffsetType != IR::MemOffsetType::UXTW))))
                        state.unsupported = true;
                    break;
                }
                // REP MOVS/STOS: one observation per side; the observer derives the range.
                case IR::OP_MEMCPY: {
                    const auto* x = op->C<IR::IROp_MemCpy>();
                    const auto size = static_cast<std::uint64_t>(IR::OpSizeToSize(x->Size));
                    accesses.push_back({node, x->Dest, size, 2});
                    accesses.back().bulk = true;
                    accesses.push_back({node, x->Src, size, 1});
                    accesses.back().bulk = true;
                    break;
                }
                case IR::OP_MEMSET: {
                    const auto* x = op->C<IR::IROp_MemSet>();
                    accesses.push_back({node, x->Addr, static_cast<std::uint64_t>(IR::OpSizeToSize(x->Size)), 2});
                    accesses.back().bulk = true;
                    accesses.back().offset = x->Prefix; // segment base, added as an SXTX offset
                    break;
                }
                case IR::OP_VSTORENONTEMPORAL: {
                    const auto* x = op->C<IR::IROp_VStoreNonTemporal>();
                    accesses.push_back({node, x->Addr, bytes, 2});
                    accesses.back().immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(x->Offset));
                    break;
                }
                case IR::OP_VSTORENONTEMPORALPAIR: {
                    const auto* x = op->C<IR::IROp_VStoreNonTemporalPair>();
                    accesses.push_back({node, x->Addr, static_cast<std::uint64_t>(bytes) * 2, 2});
                    accesses.back().immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(x->Offset));
                    break;
                }
                case IR::OP_VLOADNONTEMPORAL: {
                    const auto* x = op->C<IR::IROp_VLoadNonTemporal>();
                    accesses.push_back({node, x->Addr, bytes, 1});
                    accesses.back().immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(x->Offset));
                    break;
                }
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
                if (state.unsupported) {
                    // Keep the other accesses of the block observed; drop this op's.
                    accesses.resize(before);
                    ++state.skipped;
                }
                state.unsupported |= was_unsupported;
                for (auto i = before; i < accesses.size(); ++i)
                    accesses[i].rip = rip;
            }
        const auto saved = e->GetWriteCursor();
        bool inserted = false;
        const auto constant = [&](IR::OrderedNodeWrapper value) {
            return e->Constant(e->GetOpHeader(value)->C<IR::IROp_InlineConstant>()->Constant);
        };
        const auto observe = [&](const Access& access) {
            IR::Ref address = e->IsValueInlineConstant(access.address) ? constant(access.address)
                                                                       : e->UnwrapNode(access.address);
            if (!access.offset.IsInvalid()) {
                auto offset = e->UnwrapNode(access.offset);
                if (e->IsValueInlineConstant(access.offset))
                    offset = constant(access.offset);
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
            if (access.immediate)
                address = e->_Add(IR::OpSize::i64Bit, address, e->Constant(access.immediate)).Node;
            if (access.push)
                address = e->_Sub(IR::OpSize::i64Bit, address, e->Constant(access.bytes)).Node;
            e->_DebugMemoryAccess(address, access.bytes,
                                  access.mask | (access.bulk ? kWatchBulkAccess : 0) |
                                      (access.rip << kWatchRipShift));
            ++state.inserted;
            inserted = true;
        };
        for (const auto& access : accesses) {
            e->SetWriteCursorBefore(access.node);
            observe(access);
        }
        e->SetWriteCursor(saved);
        // Keep 64-bit values out of the 32-bit spilled PF/AF registers across the calls.
        if (inserted)
            IsolateRawFlagStores(e);
    }

private:
    MemoryWatchCompileState& state;
};
} // namespace Core::GuestCpu::Fex
