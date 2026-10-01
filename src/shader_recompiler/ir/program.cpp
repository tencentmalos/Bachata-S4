// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <fmt/format.h>

#include "common/io_file.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/value.h"

namespace Shader::IR {

void DumpProgram(const Program& program, const Info& info, const std::string& type) {
    using namespace Common::FS;

    bool dump = EmulatorSettings.IsDumpShaders();
#ifdef __ANDROID__
    // Keep IR diagnostics paired with PipelineCache's raw/SPIR-V shader dump.
    char property[PROP_VALUE_MAX]{};
    dump |= __system_property_get("debug.shadps4.shader_dump", property) > 0 &&
            std::string_view(property) == "1";
#endif
    if (!dump) {
        return;
    }

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto ir_filename =
        fmt::format("{}_{:#018x}.{}irprogram.txt", info.hw_stage, info.pgm_hash, type);
    const auto ir_file = IOFile{dump_dir / ir_filename, FileAccessMode::Create, FileType::TextFile};

    size_t index{0};
    std::map<const IR::Inst*, size_t> inst_to_index;
    std::map<const IR::Block*, size_t> block_to_index;

    for (const IR::Block* const block : program.blocks) {
        block_to_index.emplace(block, index);
        ++index;
    }

    for (const auto& block : program.blocks) {
        std::string s = IR::DumpBlock(*block, block_to_index, inst_to_index, index) + '\n';
        ir_file.WriteString(s);
    }

    const auto asl_filename =
        fmt::format("{}_{:#018x}.{}asl.txt", info.hw_stage, info.pgm_hash, type);
    const auto asl_file =
        IOFile{dump_dir / asl_filename, FileAccessMode::Create, FileType::TextFile};

    for (const auto& node : program.syntax_list) {
        std::string s = IR::DumpASLNode(node, block_to_index, inst_to_index) + '\n';
        asl_file.WriteString(s);
    }

#ifdef __ANDROID__
    // Bounded, opt-in input evidence for resource-discovery failures. Never dereference a
    // guest pointer directly: the SRT reader holds the corresponding mapping leases.
    if (type == "pre-res-discover.") {
        const auto input_file =
            IOFile{dump_dir / fmt::format("{}_{:#018x}.inputs.txt", info.hw_stage, info.pgm_hash),
                   FileAccessMode::Create, FileType::TextFile};
        for (size_t i = 0; i < info.user_data.size(); ++i)
            input_file.WriteString(fmt::format("ud[{}]={:#010x}\n", i, info.user_data[i]));
        SrtGuestReader reader;
        std::map<const Inst*, std::optional<u32>> values;
        std::function<std::optional<u32>(Value, u32)> evaluate;
        evaluate = [&](Value value, u32 depth) -> std::optional<u32> {
            if (depth > 32 || value.IsEmpty())
                return {};
            if (value.IsImmediate())
                return value.Type() == Type::U32 ? std::optional{value.U32()} : std::nullopt;
            auto* inst = value.Inst();
            if (const auto it = values.find(inst); it != values.end())
                return it->second;
            values[inst] = std::nullopt;
            std::optional<u32> result;
            if (inst->GetOpcode() == Opcode::GetUserData) {
                const auto reg = static_cast<u32>(inst->Arg(0).ScalarReg());
                if (reg < info.user_data.size())
                    result = info.user_data[reg];
            } else if (inst->GetOpcode() == Opcode::ReadConst ||
                       inst->GetOpcode() == Opcode::ReadConstBuffer) {
                const auto* pointer = inst->Arg(0).TryInst();
                if (pointer && pointer->NumArgs() >= 2) {
                    const auto lo = evaluate(pointer->Arg(0), depth + 1);
                    const auto hi = evaluate(pointer->Arg(1), depth + 1);
                    const auto offset = evaluate(inst->Arg(1), depth + 1);
                    if (lo && hi && offset) {
                        const u64 address = ((u64(*hi & 0xffff) << 32) | *lo) + u64(*offset) * 4;
                        u32 word{};
                        if (reader(address, &word, sizeof(word))) {
                            result = word;
                            input_file.WriteString(
                                fmt::format("memory {:#x}={:#010x}\n", address, word));
                        }
                    }
                }
            }
            values[inst] = result;
            return result;
        };
        std::set<std::array<u32, 4>> buffers;
        size_t budget = 4 * 1024 * 1024;
        for (const auto* block : program.blocks) {
            for (const auto& inst : block->Instructions()) {
                if (!Optimization::IsBufferInstruction(inst))
                    continue;
                auto* handle = inst.Arg(0).TryInst();
                if (!handle || handle->NumArgs() != 4)
                    continue;
                std::array<u32, 4> words{};
                bool valid = true;
                for (size_t i = 0; i < words.size(); ++i) {
                    const auto value = evaluate(handle->Arg(i), 0);
                    if (!value) {
                        valid = false;
                        break;
                    }
                    words[i] = *value;
                }
                if (!valid || !buffers.insert(words).second)
                    continue;
                const auto buffer = std::bit_cast<AmdGpu::Buffer>(words);
                input_file.WriteString(fmt::format(
                    "buffer address={:#x} stride={} records={} words={:#x},{:#x},{:#x},{:#x}\n",
                    u64(buffer.base_address), buffer.GetStride(), buffer.num_records, words[0],
                    words[1], words[2], words[3]));
                // A resource heap is a structured buffer; raw scene data only needs a prefix.
                const size_t limit =
                    buffer.GetStride() >= 16 && buffer.GetStride() <= 64 ? 2 * 1024 * 1024 : 256;
                const size_t size = std::min<size_t>({buffer.GetSize(), budget, limit});
                std::vector<u32> data(size / 4);
                budget -= data.size() * 4;
                input_file.WriteString(
                    fmt::format("  capture_bytes={} zero_words_omitted=true\n", data.size() * 4));
                for (size_t i = 0; i < data.size(); i += 64) {
                    const auto bytes = std::min<size_t>(64, data.size() - i) * 4;
                    if (!reader(buffer.base_address + i * 4, data.data() + i, bytes))
                        input_file.WriteString(
                            fmt::format("  unreadable +{:#x} size={}\n", i * 4, bytes));
                }
                for (size_t i = 0; i < data.size(); ++i)
                    if (data[i])
                        input_file.WriteString(fmt::format("  +{:#x}={:#010x}\n", i * 4, data[i]));
            }
        }
    }
#endif
}

} // namespace Shader::IR
