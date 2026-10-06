// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <initializer_list>
#include <map>
#include <memory_resource>
#include <span>
#include <vector>
#include <boost/intrusive/list.hpp>

#include "common/object_pool.h"
#include "common/types.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/ssa.h"
#include "shader_recompiler/ir/value.h"

namespace Shader::Gcn {
struct Block;
}

namespace Shader::IR {

/// The instructions of one translation and the memory of their use lists, released together.
/// Every operand of every instruction adds a use-list node, and passes such as the SSA rewrite
/// replace them many times over: taking them from a pool instead of the heap keeps large
/// shaders from spending most of their translation in malloc and free. Single-threaded, like
/// the rest of a translation.
struct InstPool {
    explicit InstPool(size_t chunk_size) : insts{chunk_size} {}

    void ReleaseContents() {
        insts.ReleaseContents();
        uses.release();
    }

    std::pmr::unsynchronized_pool_resource uses; ///< Declared first: outlives the instructions.
    Common::ObjectPool<Inst> insts;
};

class Block {
public:
    using InstructionList = boost::intrusive::list<Inst>;
    using size_type = InstructionList::size_type;
    using iterator = InstructionList::iterator;
    using const_iterator = InstructionList::const_iterator;
    using reverse_iterator = InstructionList::reverse_iterator;
    using const_reverse_iterator = InstructionList::const_reverse_iterator;

    explicit Block(InstPool& inst_pool_);
    ~Block();

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    Block(Block&&) = default;
    Block& operator=(Block&&) = default;

    void AppendNewInst(Opcode op, std::initializer_list<Value> args);

    iterator PrependNewInst(iterator insertion_point, const Inst& base_inst);
    iterator PrependNewInst(iterator insertion_point, Opcode op,
                            std::initializer_list<Value> args = {}, u32 flags = 0);

    void AddBranch(Block* block);

    [[nodiscard]] InstructionList& Instructions() noexcept {
        return instructions;
    }
    [[nodiscard]] const InstructionList& Instructions() const noexcept {
        return instructions;
    }

    [[nodiscard]] std::span<Block* const> ImmPredecessors() const noexcept {
        return imm_predecessors;
    }
    [[nodiscard]] std::span<Block* const> ImmSuccessors() const noexcept {
        return imm_successors;
    }

    template <typename T>
    void SetDefinition(T def) {
        definition = std::bit_cast<u32>(def);
    }

    template <typename T>
    [[nodiscard]] T Definition() const noexcept {
        return std::bit_cast<T>(definition);
    }

    [[nodiscard]] bool empty() const {
        return instructions.empty();
    }
    [[nodiscard]] size_type size() const {
        return instructions.size();
    }

    [[nodiscard]] Inst& front() {
        return instructions.front();
    }
    [[nodiscard]] const Inst& front() const {
        return instructions.front();
    }

    [[nodiscard]] Inst& back() {
        return instructions.back();
    }
    [[nodiscard]] const Inst& back() const {
        return instructions.back();
    }

    [[nodiscard]] iterator begin() {
        return instructions.begin();
    }
    [[nodiscard]] const_iterator begin() const {
        return instructions.cbegin();
    }
    [[nodiscard]] iterator end() {
        return instructions.end();
    }
    [[nodiscard]] const_iterator end() const {
        return instructions.cend();
    }

    [[nodiscard]] reverse_iterator rbegin() {
        return instructions.rbegin();
    }
    [[nodiscard]] const_reverse_iterator rbegin() const {
        return instructions.rbegin();
    }
    [[nodiscard]] reverse_iterator rend() {
        return instructions.rend();
    }
    [[nodiscard]] const_reverse_iterator rend() const {
        return instructions.rend();
    }

    [[nodiscard]] const_iterator cbegin() const {
        return instructions.cbegin();
    }
    [[nodiscard]] const_iterator cend() const {
        return instructions.cend();
    }

    [[nodiscard]] const_reverse_iterator crbegin() const {
        return instructions.crbegin();
    }
    [[nodiscard]] const_reverse_iterator crend() const {
        return instructions.crend();
    }

    SsaState ssa_state;
    const Shader::Gcn::Block* cfg_block{};
    U1 branch_cond{};

    InstPool* inst_pool;
    InstructionList instructions;

    Block* immediate_dominator{};
    std::vector<Block*> dominance_frontiers;
    u32 po_index{};

    std::vector<Block*> imm_predecessors;
    std::vector<Block*> imm_successors;

    u32 definition{};
};

using BlockList = std::vector<Block*>;

[[nodiscard]] std::string DumpBlock(const Block& block);

[[nodiscard]] std::string DumpBlock(const Block& block,
                                    const std::map<const Block*, size_t>& block_to_index,
                                    std::map<const Inst*, size_t>& inst_to_index,
                                    size_t& inst_index);

} // namespace Shader::IR
