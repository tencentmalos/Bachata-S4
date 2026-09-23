// SPDX-License-Identifier: GPL-2.0-or-later
#include <bit>
#include <cstdio>
#include <fstream>
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
namespace Shader {
void EmitControlFlowGraph(IR::Program&, Pools&, Gcn::CFG&, RuntimeInfo&, const Profile&);
}
using namespace Shader;
static u32 checks{}, fails{};
static void Check(bool v, const char* name) {
    ++checks;
    if (!v) {
        ++fails;
        printf("FAIL %s\n", name);
    }
}
struct Fixture {
    Info info{};
    RuntimeInfo runtime{};
    Profile profile{};
    Pools pools{};
    IR::Program p{info};
    IR::Block* b = pools.block_pool.Create(pools.inst_pool);
    Fixture() {
        info.stage = Stage::Fragment;
        info.l_stage = LogicalStage::Fragment;
        runtime.Initialize(Stage::Fragment);
        runtime.num_allocated_vgprs = 64;
        p.blocks = {b};
        p.post_order_blocks = {b};
    }
    void SSA() {
        Optimization::SsaRewritePass(p);
        Optimization::ConstantPropagationPass(p.post_order_blocks);
    }
};
static Gcn::GcnInst DS(u32 opcode, u32 off0, u32 off1, u32 addr, u32 data0, u32 data1, u32 dst) {
    const std::array<u32, 2> words{0xd8000000u | (opcode << 18) | (off1 << 8) | off0,
                                   addr | (data0 << 8) | (data1 << 16) | (dst << 24)};
    Gcn::GcnCodeSlice s(words.data(), words.data() + 2);
    Gcn::GcnDecodeContext d;
    return d.decodeInstruction(s);
}
static void Pair(u32 stride, u32 off0, u32 off1, bool st64, bool single_read) {
    Fixture f;
    Gcn::Translator t(f.info, f.runtime, f.profile);
    t.EmitPrologue(f.b);
    IR::IREmitter ir{*f.b};
    // Match the production mbcnt/u24 multiply, not a fabricated flat offset.
    auto lane = ir.BitFieldExtract(ir.LaneId(), ir.Imm32(0), ir.Imm32(24));
    auto addr = ir.IMul(lane, ir.Imm32(stride));
    ir.SetVectorReg(IR::VectorReg::V1, addr);
    ir.SetVectorReg(IR::VectorReg::V2, ir.Imm32(0x3e800000u));
    ir.SetVectorReg(IR::VectorReg::V3, ir.Imm32(0x3f400000u));
    t.TranslateInstruction(DS(st64 ? 15 : 14, off0, off1, 1, 2, 3, 0));
    // Force both source VGPRs to change: loads must recover the stored values.
    ir.SetVectorReg(IR::VectorReg::V2, ir.Imm32(0xdeadbeefu));
    ir.SetVectorReg(IR::VectorReg::V3, ir.Imm32(0xdeadbeefu));
    if (single_read) {
        const auto a = off0 * 4 * (st64 ? 64 : 1), b = off1 * 4 * (st64 ? 64 : 1);
        t.TranslateInstruction(DS(54, a & 255, a >> 8, 1, 0, 0, 4));
        t.TranslateInstruction(DS(54, b & 255, b >> 8, 1, 0, 0, 5));
    } else
        t.TranslateInstruction(DS(st64 ? 56 : 55, off0, off1, 1, 0, 0, 4));
    ir.SetAttribute(IR::Attribute::RenderTarget0,
                    ir.BitCast<IR::F32>(ir.GetVectorReg(IR::VectorReg::V4)), 0);
    ir.SetAttribute(IR::Attribute::RenderTarget0,
                    ir.BitCast<IR::F32>(ir.GetVectorReg(IR::VectorReg::V5)), 1);
    f.SSA();
    Check(Optimization::FragmentLdsPass(f.p), "private pair accepted");
    Optimization::ConstantPropagationPass(f.p.post_order_blocks);
    u32 seen = 0;
    for (auto& i : f.b->Instructions()) {
        if (i.GetOpcode() == IR::Opcode::SetAttribute) {
            auto v = i.Arg(1);
            const u32 expected = seen++ == 0 || off0 == off1 ? 0x3e800000u : 0x3f400000u;
            Check(v.IsImmediate() && std::bit_cast<u32>(v.F32()) == expected, "pair value/alias");
        }
        Check(i.GetOpcode() != IR::Opcode::LoadSharedU32 &&
                  i.GetOpcode() != IR::Opcode::WriteSharedU32,
              "no fragment shared storage");
    }
    Check(seen == 2, "two observable results");
}
static void Reject(u32 mode) {
    Fixture f;
    IR::IREmitter ir{*f.b};
    auto lane = ir.LaneId();
    auto addr = ir.IMul(lane, ir.Imm32(4));
    ir.WriteShared(32, ir.Imm32(1u), addr);
    if (mode == 0)
        ir.WriteShared(32, ir.Imm32(2u), ir.IAdd(addr, ir.Imm32(4u))); // next lane
    if (mode == 1)
        ir.WriteShared(32, ir.Imm32(2u), ir.GetAttributeU32(IR::Attribute::PrimitiveId));
    if (mode == 2)
        ir.WriteShared(32, ir.Imm32(2u), ir.IAdd(addr, ir.Imm32(2u)));
    if (mode == 3)
        ir.WriteShared(16, ir.Imm16(2), addr);
    if (mode == 4)
        ir.WriteShared(32, ir.Imm32(2u), ir.IMul(ir.BitwiseAnd(lane, ir.Imm32(1)), ir.Imm32(4)));
    const auto before = f.b->size();
    Check(!Optimization::FragmentLdsPass(f.p), "unsafe LDS refused");
    Check(f.b->size() == before && f.p.next_reg_index == 0, "refusal transactional");
}
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    for (bool scalar : {false, true}) {
        Pair(12, 0, 1, false, scalar);
        Pair(12, 2, 192, false, scalar);
        Pair(12, 193, 194, false, scalar);
        Pair(4, 0, 1, true, scalar);
        Pair(4, 3, 255, true, scalar);
        Pair(4, 7, 7, true, scalar);
    }
    for (u32 mode = 0; mode < 5; ++mode)
        Reject(mode);
    {
        Fixture f;
        IR::IREmitter ir{*f.b};
        ir.WriteShared(32, ir.Imm32(9u), ir.Imm32(0u), true);
        Check(Optimization::FragmentLdsPass(f.p), "GDS is not private LDS");
        Check(f.b->back().GetOpcode() == IR::Opcode::WriteSharedU32, "GDS untouched");
    }
    if (argc == 2) {
        std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
        auto size = in.tellg();
        in.seekg(0);
        std::vector<u32> code(size_t(size) / 4);
        in.read((char*)code.data(), size);
        Fixture f;
        f.runtime.fs_info.addr_flags.persp_center_ena = 1;
        f.p.blocks.clear();
        Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
        Gcn::GcnDecodeContext decoder;
        while (!slice.atEnd())
            f.p.ins_list.push_back(decoder.decodeInstruction(slice));
        Common::ObjectPool<Gcn::Block> blocks{64};
        Gcn::CFG cfg{blocks, f.p.ins_list};
        EmitControlFlowGraph(f.p, f.pools, cfg, f.runtime, f.profile);
        f.SSA();
        Check(Optimization::FragmentLdsPass(f.p), "captured shader private LDS proof");
        printf("captured fragment slots=%u blocks=%zu\n", f.p.next_reg_index, f.p.blocks.size());
    }
    printf("FRAGMENT_LDS %u checks / %u failures\n", checks, fails);
    return fails ? 1 : 0;
}
