// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <string>

#include <fmt/format.h>

#include "common/logging/classes.h"
#include "common/logging/log.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace Shader {

namespace {

/// Wall time of each translation step; logged when the whole translation is slow (it runs on the
/// GPU command thread the first time a shader is seen).
class StepTimer {
public:
    void operator()(const char* name) {
        const auto now = Clock::now();
        if (count < steps.size()) {
            steps[count++] = {name, now - last};
        }
        last = now;
    }

    void LogIfSlow(const IR::Program& program, const Info& info) const {
        const auto total = last - start;
        if (total < std::chrono::milliseconds{20}) {
            return;
        }
        std::size_t ir_insts = 0;
        for (const auto* block : program.blocks) {
            ir_insts += block->Instructions().size();
        }
        std::string text;
        for (u32 i = 0; i < count; ++i) {
            const double ms = std::chrono::duration<double, std::milli>(steps[i].second).count();
            if (ms >= 0.5) {
                text += fmt::format(" {} {:.1f}", steps[i].first, ms);
            }
        }
        LOG_INFO(Render_Recompiler,
                 "Translation of {} shader {:#x}: {:.1f} ms, {} GCN instructions, {} IR blocks, {} "
                 "IR instructions; steps (ms):{}",
                 info.hw_stage, info.pgm_hash,
                 std::chrono::duration<double, std::milli>(total).count(),
                 program.ins_list.size(), program.blocks.size(), ir_insts, text);
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point start{Clock::now()};
    Clock::time_point last{start};
    std::array<std::pair<const char*, Clock::duration>, 48> steps{};
    u32 count{};
};

} // namespace

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

void EmitControlFlowGraph(IR::Program& program, Pools& pools, Gcn::CFG& cfg,
                          RuntimeInfo& runtime_info, const Profile& profile) {
    Gcn::Translator translator{program.info, runtime_info, profile};
    translator.FindLiveMask(program.ins_list);
    for (auto& block : cfg) {
        const u32 start = block.begin_index;
        const u32 size = block.end_index - start + 1;
        auto* ir_block = pools.block_pool.Create(pools.inst_pool);
        ir_block->cfg_block = &block;
        block.ir_block = ir_block;
        translator.Translate(ir_block, block.begin, block.cond,
                             std::span{program.ins_list}.subspan(start, size));
        program.blocks.push_back(ir_block);
    }
    translator.EmitPrologue(program.blocks.front());
    ASSERT_MSG(!program.info.translation_failed, "Shader translation has failed");
    for (auto& block : cfg) {
        auto* ir_block = block.ir_block;
        if (block.branch_true) {
            auto* true_block = block.branch_true->ir_block;
            ir_block->AddBranch(true_block);
        }
        if (block.branch_false) {
            auto* false_block = block.branch_false->ir_block;
            ir_block->AddBranch(false_block);
        }
    }
    program.post_order_blocks = Shader::IR::PostOrder(program.blocks.front());
}

IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    StepTimer step;
    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    while (!slice.atEnd()) {
        program.ins_list.emplace_back(decoder.decodeInstruction(slice));
    }
    step("decode");

    // Clear any previous pooled data.
    pools.ReleaseContents();

    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};
    step("cfg");
    EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);
    step("translate");

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    Shader::InjectClipDistanceAttributes(program, runtime_info);
    step("InjectClipDistance");

    // Run optimization passes on unstructured graph
    if (!profile.support_float64) {
        Shader::Optimization::LowerFp64ToFp32(program);
    }
    step("LowerFp64ToFp32");
    Shader::Optimization::SsaRewritePass(program);
    step("SsaRewritePass");
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    step("ConstantPropagationPass");
    Shader::Optimization::ReadLaneEliminationPass(program);
    step("ReadLaneEliminationPass");
    ASSERT_MSG(Shader::Optimization::FragmentLdsPass(program),
               "Unsupported fragment LDS addressing in shader {:#x}", info.pgm_hash);
    step("FragmentLdsPass");
    if (info.sw_stage == SwStage::TessellationControl) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::HullShaderTransform(program, runtime_info);
    } else if (info.sw_stage == SwStage::TessellationEval) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::DomainShaderTransform(program, runtime_info);
    }
    step("Tessellation");
    Shader::Optimization::RingAccessElimination(program, runtime_info);
    step("RingAccessElimination");
    Shader::IR::DumpProgram(program, info, "pre-res-discover.");
    auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    step("ResourceDiscoverPass");
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    step("FlattenExtendedUserdataPass");
    Shader::IR::DumpProgram(program, info, "pre-res-patch.");
    Shader::Optimization::ResourcePatchingPass(program.info, resources, profile);
    step("ResourcePatchingPass");
    Shader::Optimization::LowerBufferFormatToRaw(program);
    step("LowerBufferFormatToRaw");
    Shader::Optimization::SharedMemorySimplifyPass(program, profile);
    step("SharedMemorySimplifyPass");
    Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile);
    step("SharedMemoryToStoragePass");
    Shader::Optimization::LowerUserClipPlanes(program, runtime_info);
    step("LowerUserClipPlanes");
    Shader::Optimization::PhiSimplificationPass(program);
    step("PhiSimplificationPass");
    Shader::Optimization::InverseBallotEliminationPass(program);
    step("InverseBallotEliminationPass");
    Shader::IR::DumpProgram(program, info, "pre-lower-phi.");

    // Prepare for structurization by clearing flow graph and lowering phis
    for (auto* ir_block : program.blocks) {
        ir_block->imm_predecessors.clear();
        ir_block->imm_successors.clear();
        ir_block->ssa_state.Reset();
    }
    Shader::Optimization::LowerPhisToRegsPass(program);
    step("LowerPhisToRegsPass");

    // Structurize control flow graph and create program.
    program.syntax_list = Shader::Gcn::BuildASL(pools, cfg, info);
    step("BuildASL");
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front().data.block);
    step("GenerateBlocks");

    // Run optimization passes on structured graph
    Shader::Optimization::SsaRepairPass(program);
    step("SsaRepairPass");
    Shader::Optimization::SsaRewritePass(program);
    step("SsaRewritePass#2");
    Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);
    step("SharedMemoryBarrierPass");
    Shader::Optimization::DeadCodeEliminationPass(program);
    step("DeadCodeEliminationPass");
    Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile);
    step("LowerWave64BallotPass");
    Shader::Optimization::LowerHardwareIntrinsics(program);
    step("LowerHardwareIntrinsics");
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    step("ConstantPropagationPass#2");
    Shader::Optimization::DeadCodeEliminationPass(program);
    step("DeadCodeEliminationPass#2");
    Shader::Optimization::CollectShaderInfoPass(program, profile);
    step("CollectShaderInfoPass");
    Shader::IR::DumpProgram(program, info);

    step.LogIfSlow(program, info);
    return program;
}

} // namespace Shader
