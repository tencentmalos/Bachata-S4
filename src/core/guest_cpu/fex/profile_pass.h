// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include "core/guest_cpu/api/probes.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"
#include <FEXCore/Core/CoreState.h>

namespace Core::GuestCpu::Fex {
class ProfilePass final : public FEXCore::IR::Pass {
public:
    explicit ProfilePass(std::shared_ptr<ExecutionProbes> table) : probes(std::move(table)) {}
    void Run(FEXCore::IR::IREmitter* e) override {
        namespace IR = FEXCore::IR;
        const auto view = e->ViewIR();
        const auto base = view.GetHeader()->OriginalRIP;
        const auto saved = e->GetWriteCursor();
        const auto& sites = probes->Sites();
        // Collect first: inserting nodes must not invalidate this traversal.
        std::vector<std::pair<IR::Ref, std::uint64_t>> pending;
        for (auto [block_node, block] : view.GetBlocks())
            for (auto [node, op] : view.GetCode(block_node)) {
                if (op->Op != IR::OP_GUESTOPCODE) continue;
                const auto pc = base + op->C<IR::IROp_GuestOpcode>()->GuestEntryOffset;
                auto it = std::lower_bound(sites.begin(), sites.end(), pc,
                    [](const auto& s, auto value) { return s.pc < value; });
                if (it != sites.end() && it->pc == pc && probes->ValidateSite(pc))
                    for (; it != sites.end() && it->pc == pc; ++it) pending.emplace_back(node, it->id);
            }
        for (const auto& [node, id] : pending) {
            e->SetWriteCursorBefore(node);
            auto sp = e->_LoadRegister(FEXCore::X86State::REG_RSP,
                                      IR::RegClass::GPR, IR::OpSize::i64Bit);
            e->_GuestProfileProbe(sp, id);
        }
        e->SetWriteCursor(saved);
    }
private:
    std::shared_ptr<ExecutionProbes> probes;
};
}
