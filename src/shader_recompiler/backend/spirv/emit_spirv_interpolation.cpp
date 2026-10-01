// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <stdexcept>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv_interpolation.h"
namespace Shader::Backend::SPIRV {
SoftwareInterpolationLayout MakeSoftwareInterpolationLayout(const Info& info,
                                                            const HwFragmentRuntimeInfo& fs) {
    SoftwareInterpolationLayout p;
    // Compact the fragment interface; the GS still reads original VS locations.
    for (u32 i = 0; i < fs.num_inputs; ++i) {
        if (fs.inputs[i].IsDefault() || !info.loads.GetAny(IR::Attribute::Param0 + i))
            continue;
        p.locations[i] = p.location_count;
        const bool per_vertex = info.fs_interpolation[i].primary == Qualifier::PerVertex;
        p.location_count += per_vertex ? 3 : 1;
        p.needed |= per_vertex;
    }
    const auto has = [&](IR::Attribute a) { return info.loads.GetAny(a); };
    if (has(IR::Attribute::BaryCoordSmooth) || has(IR::Attribute::BaryCoordSmoothCentroid) ||
        has(IR::Attribute::BaryCoordSmoothSample) || has(IR::Attribute::BaryCoordPullModel))
        p.smooth = p.location_count++;
    if (has(IR::Attribute::BaryCoordNoPersp) || has(IR::Attribute::BaryCoordNoPerspCentroid))
        p.linear = p.location_count++;
    if (has(IR::Attribute::BaryCoordNoPerspSample))
        p.linear_sample = p.location_count++;
    p.needed |= p.smooth.has_value() || p.linear.has_value() || p.linear_sample.has_value();
    return p;
}
namespace {
using Sirit::Id;
struct Emitter : Sirit::Module {
    std::vector<Id> interfaces;
    Id f = TypeFloat(32), u = TypeUInt(32), v3 = TypeVector(f, 3), v4 = TypeVector(f, 4);
    Id N(u32 n) {
        return Constant(u, n);
    }
    Id Var(Id type, spv::StorageClass storage) {
        auto id = AddGlobalVariable(TypePointer(storage, type), storage);
        interfaces.push_back(id);
        return id;
    }
    Id Input(Id type) {
        return Var(type, spv::StorageClass::Input);
    }
    Id Output(Id type) {
        return Var(type, spv::StorageClass::Output);
    }
    Id Ptr(Id type, spv::StorageClass storage) {
        return TypePointer(storage, type);
    }
    struct Varying {
        Id in, out;
        bool per_vertex;
    };
    explicit Emitter(const Info& vertex, const RuntimeInfo& vs, const Info& fragment,
                     const HwFragmentRuntimeInfo& fs, const SoftwareInterpolationLayout& p)
        : Sirit::Module(0x00010500) {
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::Geometry);
        // The first implementation deliberately rejects builtins/topologies which
        // require a different interface instead of silently dropping their values.
        if (fs.clip_distance_emulation || vertex.stores.GetAny(IR::Attribute::CullDistance) ||
            vertex.stores.GetAny(IR::Attribute::PointSize) ||
            vertex.stores.GetAny(IR::Attribute::RenderTargetIndex) ||
            vertex.stores.GetAny(IR::Attribute::ViewportIndex))
            throw std::runtime_error("software interpolation: unsupported vertex builtin");
        const bool depth =
            vs.depth_range.enabled && (vs.depth_range.clip_near || vs.depth_range.clip_far);
        const bool clip = depth || vertex.stores.GetAny(IR::Attribute::ClipDistance);
        const Id clips = TypeArray(f, N(8));
        Id pos_in, pos_out, clip_in{}, clip_out{};
        if (depth) {
            const Id block = TypeStruct(v4, clips);
            Decorate(block, spv::Decoration::Block);
            MemberDecorate(block, 0, spv::Decoration::BuiltIn, u32(spv::BuiltIn::Position));
            MemberDecorate(block, 1, spv::Decoration::BuiltIn, u32(spv::BuiltIn::ClipDistance));
            pos_in = Input(TypeArray(block, N(3)));
            pos_out = Output(block);
        } else {
            pos_in = Input(TypeArray(v4, N(3)));
            pos_out = Output(v4);
            Decorate(pos_in, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
            Decorate(pos_out, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
            if (clip) {
                clip_in = Input(TypeArray(clips, N(3)));
                clip_out = Output(clips);
                Decorate(clip_in, spv::Decoration::BuiltIn, spv::BuiltIn::ClipDistance);
                Decorate(clip_out, spv::Decoration::BuiltIn, spv::BuiltIn::ClipDistance);
            }
        }
        if (clip)
            AddCapability(spv::Capability::ClipDistance);
        std::vector<Varying> varying;
        for (u32 i = 0; i < fs.num_inputs; ++i) {
            if (fs.inputs[i].IsDefault() || !fragment.loads.GetAny(IR::Attribute::Param0 + i))
                continue;
            const u32 location = fs.inputs[i].param_index;
            if (!vertex.stores.GetAny(IR::Attribute::Param0 + location))
                throw std::runtime_error(
                    "software interpolation: fragment input has no vertex output");
            const bool per = fragment.fs_interpolation[i].primary == Qualifier::PerVertex;
            const Id input = Input(TypeArray(v4, N(3)));
            Decorate(input, spv::Decoration::Location, location);
            const Id output = Output(per ? TypeArray(v4, N(3)) : v4);
            Decorate(output, spv::Decoration::Location, p.locations[i]);
            if (per || fragment.fs_interpolation[i].primary == Qualifier::Flat)
                Decorate(output, spv::Decoration::Flat);
            varying.push_back({input, output, per});
        }
        std::vector<Id> basis_outputs;
        for (auto location : {p.smooth, p.linear, p.linear_sample}) {
            if (!location)
                continue;
            const Id output = Output(v3);
            Decorate(output, spv::Decoration::Location, *location);
            if (location != p.smooth)
                Decorate(output, spv::Decoration::NoPerspective);
            basis_outputs.push_back(output);
        }
        Id primitive_in{}, primitive_out{};
        if (fragment.loads.GetAny(IR::Attribute::PrimitiveId)) {
            primitive_in = Input(u);
            primitive_out = Output(u);
            Decorate(primitive_in, spv::Decoration::BuiltIn, spv::BuiltIn::PrimitiveId);
            Decorate(primitive_out, spv::Decoration::BuiltIn, spv::BuiltIn::PrimitiveId);
        }
        const Id void_type = TypeVoid();
        const Id main =
            OpFunction(void_type, spv::FunctionControlMask::MaskNone, TypeFunction(void_type));
        AddExecutionMode(main, spv::ExecutionMode::Triangles);
        AddExecutionMode(main, spv::ExecutionMode::OutputTriangleStrip);
        AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 3U);
        AddExecutionMode(main, spv::ExecutionMode::Invocations, 1U);
        AddEntryPoint(spv::ExecutionModel::Geometry, main, "main", interfaces);
        AddLabel(OpLabel());
        const Id in_v4 = Ptr(v4, spv::StorageClass::Input),
                 out_v4 = Ptr(v4, spv::StorageClass::Output);
        const Id in_f = Ptr(f, spv::StorageClass::Input), out_f = Ptr(f, spv::StorageClass::Output);
        for (u32 v = 0; v < 3; ++v) {
            const Id src = depth ? OpAccessChain(in_v4, pos_in, N(v), N(0))
                                 : OpAccessChain(in_v4, pos_in, N(v));
            const Id dst = depth ? OpAccessChain(out_v4, pos_out, N(0)) : pos_out;
            OpStore(dst, OpLoad(v4, src));
            if (clip)
                for (u32 c = 0; c < 8; ++c) {
                    const Id a = depth ? OpAccessChain(in_f, pos_in, N(v), N(1), N(c))
                                       : OpAccessChain(in_f, clip_in, N(v), N(c));
                    const Id b = depth ? OpAccessChain(out_f, pos_out, N(1), N(c))
                                       : OpAccessChain(out_f, clip_out, N(c));
                    OpStore(b, OpLoad(f, a));
                }
            // Geometry outputs become undefined after EmitVertex: publish every
            // flat array again, so either provoking-vertex convention sees all three.
            for (const auto& a : varying) {
                if (a.per_vertex) {
                    for (u32 i = 0; i < 3; ++i)
                        OpStore(OpAccessChain(out_v4, a.out, N(i)),
                                OpLoad(v4, OpAccessChain(in_v4, a.in, N(i))));
                } else
                    OpStore(a.out, OpLoad(v4, OpAccessChain(in_v4, a.in, N(v))));
            }
            const Id basis =
                ConstantComposite(v3, Constant(f, v == 0 ? 1.f : 0.f),
                                  Constant(f, v == 1 ? 1.f : 0.f), Constant(f, v == 2 ? 1.f : 0.f));
            for (Id out : basis_outputs)
                OpStore(out, basis);
            if (primitive_in.value)
                OpStore(primitive_out, OpLoad(u, primitive_in));
            OpEmitVertex();
        }
        OpEndPrimitive();
        OpReturn();
        OpFunctionEnd();
        SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    }
};
} // namespace
std::vector<u32> EmitSoftwareInterpolationGeometry(const Info& vertex, const RuntimeInfo& vs,
                                                   const Info& fragment,
                                                   const HwFragmentRuntimeInfo& fs,
                                                   u32 max_output_components,
                                                   u32 max_total_output_components) {
    const auto plan = MakeSoftwareInterpolationLayout(fragment, fs);
    const u32 components =
        plan.location_count * 4 + 13; // Position, max clip distances, PrimitiveId.
    if (components > max_output_components || components * 3 > max_total_output_components)
        throw std::runtime_error("software interpolation exceeds geometry output limits");
    Emitter emitter(vertex, vs, fragment, fs, plan);
    return emitter.Assemble();
}
} // namespace Shader::Backend::SPIRV
