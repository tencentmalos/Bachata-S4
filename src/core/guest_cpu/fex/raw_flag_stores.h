// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vector>
#include "Interface/IR/IREmitter.h"

namespace Core::GuestCpu::Fex {
// For passes that insert calls into guest code (DebugMemoryAccess, GuestProfileProbe).
//
// FEX's register allocator may keep a value in the raw PF/AF static registers (x26/x27) when
// it is also stored as PF or AF, e.g. the result and the loaded source of `dec qword [rdi+8]`,
// and keep using that 64-bit value as data afterwards (there, the store back to memory). The
// call ops spill and refill the static registers around the host call, PF/AF as 32 bits only
// (all the flags need), so the upper half of such a value is lost across an inserted call.
// Give every StorePF/StoreAF its own copy of the value: only the copy is placed in PF/AF, and
// the value itself stays in a register the call preserves in full. One move per flag store.
inline void IsolateRawFlagStores(FEXCore::IR::IREmitter* e) {
    namespace IR = FEXCore::IR;
    std::vector<IR::Ref> stores;
    auto view = e->ViewIR();
    for (auto [block, header] : view.GetBlocks())
        for (auto [node, op] : view.GetCode(block))
            if (op->Op == IR::OP_STOREPF || op->Op == IR::OP_STOREAF)
                stores.push_back(node);
    const auto saved = e->GetWriteCursor();
    for (auto node : stores) {
        auto* op = view.GetOp<IR::IROp_Header>(node);
        const auto value = op->Op == IR::OP_STOREPF ? op->C<IR::IROp_StorePF>()->Value
                                                    : op->C<IR::IROp_StoreAF>()->Value;
        e->SetWriteCursorBefore(node);
        e->ReplaceNodeArgument(node, 0, e->_Copy(e->UnwrapNode(value)));
    }
    e->SetWriteCursor(saved);
}
} // namespace Core::GuestCpu::Fex
