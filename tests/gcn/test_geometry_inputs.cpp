// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Geometry shader inputs, after AstroQuest e856864 (references/AstroQuest,
// tests/gcn/test_geometry_inputs.cpp there): the GS wave's input registers follow the hardware
// layout whatever the input primitive, V7 carries the instance (invocation) id, and two
// geometry stages that differ in their vertex data sizes are different shaders.

#include <algorithm>
#include <array>
#include <gtest/gtest.h>

#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace {

using namespace Shader;

/// The value the prologue stores into `reg`, or nullptr when it stores none.
const IR::Inst* PrologueStore(const IR::Block& block, IR::VectorReg reg) {
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetVectorRegister && inst.Arg(0).VectorReg() == reg) {
            return inst.Arg(1).TryInst();
        }
    }
    return nullptr;
}

bool StoresImmediate(const IR::Block& block, IR::VectorReg reg, u32 value) {
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetVectorRegister && inst.Arg(0).VectorReg() == reg) {
            return inst.Arg(1).IsImmediate() && inst.Arg(1).U32() == value;
        }
    }
    return false;
}

struct Prologue {
    explicit Prologue(AmdGpu::PrimitiveType in_primitive) {
        info.hw_stage = HwStage::Geometry;
        info.sw_stage = SwStage::Geometry;
        runtime.Initialize(HwStage::Geometry, SwStage::Geometry);
        runtime.hw.gs.in_primitive = in_primitive;
        runtime.hw.gs.num_invocations = 2;
        block = pools.block_pool.Create(pools.inst_pool);
        Gcn::Translator translator{info, runtime, profile};
        translator.EmitPrologue(block);
    }

    Info info{};
    RuntimeInfo runtime{};
    Profile profile{};
    Pools pools{};
    IR::Block* block{};
};

TEST(GeometryInputs, InvocationIdReachesV7) {
    Prologue prologue{AmdGpu::PrimitiveType::TriangleList};
    const auto* value = PrologueStore(*prologue.block, IR::VectorReg::V7);
    ASSERT_NE(value, nullptr);
    ASSERT_EQ(value->GetOpcode(), IR::Opcode::GetAttributeU32);
    EXPECT_EQ(value->Arg(0).Attribute(), IR::Attribute::InvocationId);
}

TEST(GeometryInputs, VertexOffsetsFollowTheHardwareLayout) {
    // Fans and polygons used to get only V0; every input primitive gets the same layout.
    for (const auto type :
         {AmdGpu::PrimitiveType::PointList, AmdGpu::PrimitiveType::LineLoop,
          AmdGpu::PrimitiveType::TriangleFan, AmdGpu::PrimitiveType::Polygon,
          AmdGpu::PrimitiveType::AdjTriangleStrip, AmdGpu::PrimitiveType::AdjLineStrip}) {
        Prologue prologue{type};
        const auto& block = *prologue.block;
        EXPECT_TRUE(StoresImmediate(block, IR::VectorReg::V0, 0)) << u32(type);
        EXPECT_TRUE(StoresImmediate(block, IR::VectorReg::V1, 1)) << u32(type);
        EXPECT_TRUE(StoresImmediate(block, IR::VectorReg::V3, 2)) << u32(type);
        EXPECT_TRUE(StoresImmediate(block, IR::VectorReg::V4, 3)) << u32(type);
        EXPECT_TRUE(StoresImmediate(block, IR::VectorReg::V5, 4)) << u32(type);
        EXPECT_TRUE(StoresImmediate(block, IR::VectorReg::V6, 5)) << u32(type);
        const auto* primitive = PrologueStore(block, IR::VectorReg::V2);
        ASSERT_NE(primitive, nullptr) << u32(type);
        EXPECT_EQ(primitive->Arg(0).Attribute(), IR::Attribute::PrimitiveId) << u32(type);
    }
}

TEST(GeometryInputs, VertexDataSizesTellShadersApart) {
    HwGeometryRuntimeInfo a{};
    a.in_primitive = AmdGpu::PrimitiveType::TriangleList;
    a.in_vertex_data_size = 4;
    a.out_vertex_data_size = 4;
    HwGeometryRuntimeInfo b = a;
    EXPECT_TRUE(a == b);
    b.in_vertex_data_size = 8;
    EXPECT_FALSE(a == b);
    b = a;
    b.out_vertex_data_size = 8;
    EXPECT_FALSE(a == b);
    b = a;
    b.num_invocations = 2;
    EXPECT_FALSE(a == b);
}

} // namespace
