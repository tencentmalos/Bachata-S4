// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <Zydis/Zydis.h>
#include "core/host_runtime/guest_patch.h"

namespace Core::GuestPatch {
namespace {
void Require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class T>
void Append(Bytes& b, T value) {
    const auto x = std::as_bytes(std::span{&value, 1});
    b.insert(b.end(), x.begin(), x.end());
}
void Jump(Bytes& b, uint64_t target) {
    for (auto x : {0xff, 0x25, 0, 0, 0, 0})
        b.push_back(std::byte(x));
    Append(b, target);
}
struct Instruction {
    ZydisDecodedInstruction decoded{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
    size_t input{}, output{}, length{};
    uint64_t target{};
    enum Kind { Copy, Call, Jmp, Jcc } kind{Copy};
};
} // namespace
Relocated Relocate(std::span<const std::byte> bytes, uint64_t source, uint64_t dest, size_t min) {
    Require(min >= 5 && min <= 32 && bytes.size() <= 256, "invalid trampoline window");
    Require(source <= INT64_MAX - 4096 && dest <= INT64_MAX - 4096,
            "trampoline address out of range");
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    std::vector<Instruction> instructions;
    size_t input = 0, output = 0;
    while (input < min) {
        Require(input < bytes.size(), "truncated trampoline preimage");
        Instruction i;
        i.input = input;
        i.output = output;
        Require(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + input,
                                                    bytes.size() - input, &i.decoded, i.operands)),
                "invalid x86 instruction");
        auto& d = i.decoded;
        Require(d.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY,
                "trampoline requires legacy/SSE2 encoding");
        // These expose IP, interrupt state, transactions, or non-local control;
        // copying them would silently change observable semantics.
        switch (d.mnemonic) {
        case ZYDIS_MNEMONIC_CALL:
        case ZYDIS_MNEMONIC_JMP:
            break;
        case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE:
        case ZYDIS_MNEMONIC_LOOPNE:
        case ZYDIS_MNEMONIC_JRCXZ:
        case ZYDIS_MNEMONIC_JECXZ:
        case ZYDIS_MNEMONIC_RET:
        case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD:
        case ZYDIS_MNEMONIC_IRETQ:
        case ZYDIS_MNEMONIC_INT:
        case ZYDIS_MNEMONIC_INT3:
        case ZYDIS_MNEMONIC_SYSCALL:
        case ZYDIS_MNEMONIC_SYSRET:
        case ZYDIS_MNEMONIC_UD2:
        case ZYDIS_MNEMONIC_XBEGIN:
        case ZYDIS_MNEMONIC_XEND:
        case ZYDIS_MNEMONIC_FNSTENV:
        case ZYDIS_MNEMONIC_FXSAVE:
        case ZYDIS_MNEMONIC_FXSAVE64:
            throw std::runtime_error("unsupported PC/control-sensitive trampoline instruction");
        default:
            break;
        }
        i.length = d.length;
        for (unsigned o = 0; o < d.operand_count_visible; ++o) {
            const auto& operand = i.operands[o];
            if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative)
                continue;
            Require(ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&d, &operand, source + input, &i.target)),
                    "cannot resolve branch");
            if (d.mnemonic == ZYDIS_MNEMONIC_CALL) {
                Require(i.target != source + input + d.length,
                        "call/pop PC discovery is unsupported");
                i.kind = Instruction::Call;
                i.length = 16;
            } else if (d.mnemonic == ZYDIS_MNEMONIC_JMP) {
                i.kind = Instruction::Jmp;
                i.length = 14;
            } else {
                Require(d.meta.category == ZYDIS_CATEGORY_COND_BR &&
                            ((d.opcode >= 0x70 && d.opcode <= 0x7f) ||
                             (d.opcode >= 0x80 && d.opcode <= 0x8f)),
                        "unsupported relative instruction");
                i.kind = Instruction::Jcc;
                i.length = 16;
            }
        }
        instructions.push_back(i);
        input += d.length;
        output += i.length;
    }
    Relocated result;
    result.stolen = input;
    auto target = [&](uint64_t address) {
        if (address < source || address >= source + input)
            return address;
        const auto it = std::find_if(instructions.begin(), instructions.end(),
                                     [&](const auto& i) { return source + i.input == address; });
        Require(it != instructions.end(), "branch enters middle of stolen instruction");
        return dest + it->output;
    };
    for (const auto& i : instructions) {
        result.instructions.emplace_back(source + i.input, dest + i.output);
        auto& b = result.code;
        if (i.kind == Instruction::Jmp) {
            Jump(b, target(i.target));
            continue;
        }
        if (i.kind == Instruction::Jcc) {
            b.push_back(std::byte(0x70 | ((i.decoded.opcode & 15) ^ 1)));
            b.push_back(std::byte{14});
            Jump(b, target(i.target));
            continue;
        }
        if (i.kind == Instruction::Call) {
            // call [rip+2]; jmp +8; target. No scratch GPR, preserves AL and flags.
            for (auto x : {0xff, 0x15, 2, 0, 0, 0, 0xeb, 8})
                b.push_back(std::byte(x));
            Append(b, target(i.target));
            continue;
        }
        const size_t start = b.size();
        b.insert(b.end(), bytes.begin() + i.input, bytes.begin() + i.input + i.decoded.length);
        for (unsigned o = 0; o < i.decoded.operand_count_visible; ++o) {
            const auto& operand = i.operands[o];
            if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY)
                continue;
            Require(operand.mem.base != ZYDIS_REGISTER_EIP,
                    "address-size EIP relocation unsupported");
            if (operand.mem.base != ZYDIS_REGISTER_RIP)
                continue;
            uint64_t address{};
            Require(ZYAN_SUCCESS(
                        ZydisCalcAbsoluteAddress(&i.decoded, &operand, source + i.input, &address)),
                    "invalid RIP-relative operand");
            Require(address < source || address >= source + input,
                    "RIP-relative data aliases overwritten entry");
            const auto displacement =
                int64_t(address) - int64_t(dest + i.output + i.decoded.length);
            Require(i.decoded.raw.disp.size == 32 && displacement >= INT32_MIN &&
                        displacement <= INT32_MAX,
                    "RIP-relative target outside rel32 range");
            const auto value = int32_t(displacement);
            std::memcpy(b.data() + start + i.decoded.raw.disp.offset, &value, 4);
        }
    }
    Jump(result.code, source + input);
    return result;
}
} // namespace Core::GuestPatch
