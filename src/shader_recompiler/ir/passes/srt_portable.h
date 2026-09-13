// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>
#include "common/types.h"

namespace Shader {
// Immutable, pointer-free SRT plan. No native code or IR-pool pointers survive compilation.
// Expression operands refer only backwards; bounded depth is checked before execution/cache use.
struct PortableSrt {
    enum class Op : u32 {
        Constant,
        Flat,
        Add,
        Sub,
        Mul,
        Shl,
        Shr,
        And,
        Or,
        Xor,
        Not,
        Min,
        Max,
        Extract
    };
    struct Expr {
        Op op{};
        u32 a{}, b{}, c{};
    };
    enum class Kind : u32 { Push, Copy, Pop };
    struct Command {
        Kind kind{};
        u32 expression{}, destination{};
    };
    static constexpr u32 MaxEntries = 65535;
    static constexpr u32 MaxDepth = 64;
    std::vector<Expr> expressions;
    std::vector<Command> commands;

    bool Validate(u32 flat_size) const {
        if (flat_size < 16 || flat_size > MaxEntries || expressions.size() > MaxEntries ||
            commands.size() > MaxEntries)
            return false;
        std::vector<u32> depth(expressions.size());
        for (u32 i = 0; i < expressions.size(); ++i) {
            const auto& e = expressions[i];
            const auto arg = [&](u32 a) { return a < i && depth[a] < MaxDepth; };
            switch (e.op) {
            case Op::Constant:
                depth[i] = 1;
                break;
            case Op::Flat:
                if (e.a >= flat_size)
                    return false;
                depth[i] = 1;
                break;
            case Op::Not:
                if (!arg(e.a))
                    return false;
                depth[i] = depth[e.a] + 1;
                break;
            case Op::Extract:
                if (!arg(e.a) || !arg(e.b) || !arg(e.c))
                    return false;
                depth[i] = std::max({depth[e.a], depth[e.b], depth[e.c]}) + 1;
                break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Shl:
            case Op::Shr:
            case Op::And:
            case Op::Or:
            case Op::Xor:
            case Op::Min:
            case Op::Max:
                if (!arg(e.a) || !arg(e.b))
                    return false;
                depth[i] = std::max(depth[e.a], depth[e.b]) + 1;
                break;
            default:
                return false;
            }
        }
        u32 stack{};
        for (auto command : commands) {
            switch (command.kind) {
            case Kind::Push:
                if (command.expression >= expressions.size() || ++stack > MaxDepth)
                    return false;
                break;
            case Kind::Copy:
                if (!stack || command.expression >= expressions.size() ||
                    command.destination < 16 || command.destination >= flat_size)
                    return false;
                break;
            case Kind::Pop:
                if (!stack)
                    return false;
                --stack;
                break;
            default:
                return false;
            }
        }
        return stack == 0;
    }

private:
    struct Memo {
        u32 epoch{}, value{};
    };
    u32 Evaluate(u32 id, std::span<const u32> flat, std::span<Memo> memo, u32 epoch) const {
        if (memo[id].epoch == epoch)
            return memo[id].value;
        const u32 value = Compute(id, flat, memo, epoch);
        memo[id] = {epoch, value};
        return value;
    }
    u32 Compute(u32 id, std::span<const u32> flat, std::span<Memo> memo, u32 epoch) const {
        const auto& e = expressions[id];
        if (e.op == Op::Constant)
            return e.a;
        if (e.op == Op::Flat)
            return flat[e.a];
        const u32 a = Evaluate(e.a, flat, memo, epoch);
        if (e.op == Op::Not)
            return ~a;
        const u32 b = Evaluate(e.b, flat, memo, epoch);
        switch (e.op) {
        case Op::Add:
            return a + b;
        case Op::Sub:
            return a - b;
        case Op::Mul:
            return a * b;
        case Op::Shl:
            return a << (b & 31);
        case Op::Shr:
            return a >> (b & 31);
        case Op::And:
            return a & b;
        case Op::Or:
            return a | b;
        case Op::Xor:
            return a ^ b;
        case Op::Min:
            return std::min(a, b);
        case Op::Max:
            return std::max(a, b);
        case Op::Extract: {
            const u32 count = Evaluate(e.c, flat, memo, epoch);
            return count >= 32 ? a >> (b & 31) : (a >> (b & 31)) & ((1u << count) - 1);
        }
        default:
            throw std::runtime_error("invalid portable SRT expression");
        }
    }

public:
    u32 Evaluate(u32 id, std::span<const u32> flat) const {
        std::vector<Memo> memo(expressions.size());
        return Evaluate(id, flat, memo, 1);
    }
    template <class Read>
    void Run(std::span<const u32> user_data, std::span<u32> flat, Read read) const {
        // Validate once at compile/deserialization; the immutable plan's lifetime is owned by Info.
        std::array<u64, MaxDepth> stack{};
        u32 depth{};
        u64 pointer{};
        std::vector<Memo> memo(expressions.size());
        u32 epoch{};
        for (auto command : commands) {
            if (command.kind == Kind::Pop) {
                pointer = stack[--depth];
                continue;
            }
            const u64 offset = u32(Evaluate(command.expression, flat, memo, ++epoch) << 2);
            if (command.kind == Kind::Push) {
                u64 next{};
                if (depth == 0) {
                    if (offset <= user_data.size_bytes() &&
                        sizeof(next) <= user_data.size_bytes() - offset)
                        std::memcpy(&next, reinterpret_cast<const u8*>(user_data.data()) + offset,
                                    sizeof(next));
                } else if (pointer && pointer <= UINT64_MAX - offset) {
                    if (!read(pointer + offset, &next, sizeof(next)))
                        next = 0;
                }
                stack[depth++] = pointer;
                pointer = next & 0xFFFFFFFFFFFFULL;
            } else {
                u32 value{};
                if (pointer && pointer <= UINT64_MAX - offset &&
                    !read(pointer + offset, &value, sizeof(value)))
                    value = 0;
                flat[command.destination] = value;
            }
        }
    }
};
// Uses the current MemoryManager's mapped ranges, never an arbitrary host pointer.
bool ReadSrtGuestMemory(u64 address, void* data, size_t size);
} // namespace Shader
