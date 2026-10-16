// SPDX-License-Identifier: GPL-2.0-or-later
// UE-facing responsibilities reconstructed from Tetris Release-4.26.
// Normalized objects, NOT UE headers, guest layouts, or a complete PS4 plugin.
#pragma once
#include "GnmStagesCore.h"

namespace Tetris426 { namespace Graphics {
struct Shader {
    Frequency frequency = Frequency::Vertex;
    Stage vertex_stage = Stage::Vertex; // native shader+394
    Program program;
    Reflection reflection;
    Program export_program; // vertex object+278; indirect selectors read from ES+19
    Reflection export_reflection; // vertex object+280
    Program copy_program; // GS embedded copy program, decoded separately
    GeometryMetadata geometry_metadata;
    u8 geometry_header3 = 0, copy_header3 = 1;
    ResourceMaps maps;
    std::vector<u8> static_uniform_slots; // shader+260, 255 means unused
    std::vector<u32> static_uniform_hashes; // shader+240
};
struct Pipeline {
    const Shader* vertex = nullptr;
    const Shader* pixel = nullptr;
    const Shader* geometry = nullptr;
    std::vector<VertexElement> elements; // pipeline+a0/a8, already compiled
    u32 topology = 0, modifier = 0, mode = 0;
    u64 fetch = 0;
    Stage vertex_stage = Stage::Vertex;
    u8 loose_mask = 0; // pipeline+f4: VS1/PS2/GS4
    bool needs_ring_table = false;
    u64 native_identity = 0; // platform can resolve fixed state from this
    u32 es_lds512 = 0, gs_groups = 0, es_word1c = 0;
    // Stage portion of f9d570. Caller supplies decoded native shader objects.
    void ConfigureStages() {
        Check(vertex, "pipeline vertex missing");
        if (!geometry) {
            vertex_stage = Stage::Vertex; mode = 0; needs_ring_table = false;
            es_word1c = 0; es_lds512 = gs_groups = 0; return;
        }
        vertex_stage = Stage::Export; mode = 0xb0;
        const auto& g = geometry->geometry_metadata;
        needs_ring_table = !g.OnChip();
        es_word1c = vertex->export_program.body[5];
        const auto budget = g.OnChip() ? OnChipBudget(g) : GeometryBudget{};
        es_lds512 = budget.lds512; gs_groups = budget.groups;
    }
};
struct VertexBuffer { u64 current_address = 0; u32 size = 0; };

// Required platform operations, in addition to GnmGraphicsCore's Sink contract:
// BeginPipeline(next,old): prepare backend recording; stage registers are emitted here.
// FixedState(next,old): precompiled blend/raster/depth/etc AFTER selection.
// RetainPipeline(old,next), RetainUniform(freq,slot,old,next): preserve references.
// ValidateStaticUniform(ub,global_slot,expected_hash): native 184f170 boundary.
// These callbacks have no success/no-op defaults. A real backend must supply
// native state emission and retirement, and stop on failure. Test sinks only record.
template<class Sink> class CommandContext {
public:
    Sink& backend;
    StageLcue lcue;
    RingStorage rings;
    RingTable ring_seed{}; // caller-owned auxiliary/tail words; no inferred contents
    UniformBindings uniforms;
    DrawState draw_state;
    std::array<Stream, 16> streams{};
    std::array<LooseConstants, 3> loose; // VS, PS, GS; capacities supplied by caller
    std::vector<const UniformBuffer*> static_uniforms;
    const Pipeline* pipeline = nullptr;
    bool vertex_dirty = false, discard_loose_extent = false;
    u32 first_instance = 0;

    explicit CommandContext(Sink& sink, unsigned constant_capacity)
        : backend(sink), loose{{LooseConstants(constant_capacity),
            LooseConstants(constant_capacity), LooseConstants(constant_capacity)}} {}

    static void ValidatePipeline(const Pipeline& p) {
        Check(p.vertex && p.vertex->frequency == Frequency::Vertex,
              "pipeline requires a vertex shader");
        Check(!p.pixel || p.pixel->frequency == Frequency::Pixel, "pixel frequency");
        if (p.geometry) {
            Check(p.geometry->frequency == Frequency::Geometry && p.mode == 0xb0 &&
                  p.vertex_stage == Stage::Export && p.vertex->vertex_stage == Stage::Export,
                  "GS pipeline requires ES routing");
            Check(p.vertex->export_program.identity &&
                  p.geometry->copy_program.identity == CopyShaderAddress(
                      p.geometry->program.identity, p.geometry->geometry_header3), "embedded copy address");
            Check(p.needs_ring_table == !p.geometry->geometry_metadata.OnChip(), "GS ring mode");
        } else {
            Check(!p.needs_ring_table && p.mode == 0 && p.vertex_stage == Stage::Vertex &&
                  p.vertex->vertex_stage == Stage::Vertex, "UE tess pipeline bridge not recovered");
        }
    }
    void RHISetGraphicsPipelineState(const Pipeline& next, bool apply_static_uniforms) {
        ValidatePipeline(next);
        backend.BeginPipeline(next, pipeline);
        lcue.SetMode(backend, next.mode, !pipeline);
        // f9f840 compares UE shader OBJECT pointers, not merely program code.
        // If shader objects are unchanged it skips selection, including fetch/modifier.
        if (!pipeline || pipeline->vertex != next.vertex || pipeline->geometry != next.geometry) {
            if (next.geometry) {
                const auto& gs = *next.geometry; const auto& g = gs.geometry_metadata;
                lcue.SelectES(next.vertex->export_program, next.vertex->export_reflection,
                              next.fetch, next.modifier, g.OnChip() ? next.es_lds512 : 0);
                lcue.SelectGS(backend, gs.program, gs.reflection, gs.copy_program,
                              gs.copy_header3 == 1, g, next.gs_groups);
                if (g.OnChip()) {
                    lcue.SetExportFactor(u16(next.es_word1c));
                    ContextRegister(backend, 0x2ab, next.es_word1c & 0x7fff);
                    GeometryRingRegisters(backend, g);
                }
            } else lcue.SelectVS(next.vertex->program, next.vertex->reflection, next.fetch, next.modifier);
        }
        if (!pipeline || pipeline->pixel != next.pixel)
            lcue.SelectPS(next.pixel ? &next.pixel->program : nullptr,
                          next.pixel ? &next.pixel->reflection : nullptr);
        // f9f840 compares the six layout words against the previous PSO, not a
        // hash of allocation addresses. Native ring backing is manager-owned.
        if (next.needs_ring_table && (!pipeline || !pipeline->geometry ||
            !SameRingLayout(next.es_word1c, next.geometry->geometry_metadata,
                            pipeline->needs_ring_table ? pipeline->es_word1c : 0,
                            pipeline->needs_ring_table ? pipeline->geometry->geometry_metadata : GeometryMetadata{})))
            lcue.UploadRings(backend, ring_seed, rings, next.es_word1c, next.geometry->geometry_metadata);
        backend.FixedState(next, pipeline);
        backend.RetainPipeline(pipeline, &next);
        pipeline = &next;
        vertex_dirty = discard_loose_extent = true;
        uniforms.PipelineChanged(); // no forced loose-shadow dirty
        if (apply_static_uniforms) {
            ApplyStatic(next.vertex);
            ApplyStatic(next.geometry);
            ApplyStatic(next.pixel);
        }
    }
    void ApplyStatic(const Shader* shader) {
        if (!shader) return;
        for (unsigned slot = 0; slot < shader->static_uniform_slots.size(); ++slot) {
            const u8 global = shader->static_uniform_slots[slot];
            if (global == 255) continue;
            Check(global < static_uniforms.size() && slot < shader->static_uniform_hashes.size(),
                  "static UB table range");
            const auto* ub = static_uniforms[global];
            backend.ValidateStaticUniform(ub, global, shader->static_uniform_hashes[slot]);
            if (ub) RHISetShaderUniformBuffer(*shader, slot, *ub);
        }
    }
    void RHISetStreamSource(unsigned slot, const VertexBuffer* vb, u32 offset) {
        Check(slot < streams.size(), "stream slot");
        streams[slot] = SnapshotStream(vb ? vb->current_address : 0, vb ? vb->size : 0,
                                       offset, vb != nullptr);
        vertex_dirty = true;
    }
    void RHISetShaderUniformBuffer(const Shader& s, unsigned slot, const UniformBuffer& ub) {
        Check(Index(s.frequency) < 5 && slot < 20, "uniform slot");
        const auto* old = uniforms.buffers[Index(s.frequency)][slot];
        uniforms.Set(backend, lcue, s.frequency, HardwareStage(s.frequency, s.vertex_stage), slot, ub);
        backend.RetainUniform(s.frequency, slot, old, &ub);
    }
    void RHISetShaderTexture(const Shader& s, unsigned slot, const Resource* texture) {
        if (!texture) return; // does not unbind
        Check(texture->kind == Resource::Kind::Texture, "texture resource");
        const auto stage = HardwareStage(s.frequency, s.vertex_stage);
        lcue.stages[Index(stage)].Bind(backend, stage, Binding::ReadTexture, slot, texture->words.data());
    }
    void RHISetShaderSampler(const Shader& s, unsigned slot, const Resource& sampler) {
        Check(sampler.kind == Resource::Kind::Sampler, "sampler resource");
        const auto stage = HardwareStage(s.frequency, s.vertex_stage);
        lcue.stages[Index(stage)].Bind(backend, stage, Binding::Sampler, slot, sampler.words.data());
    }
    void RHISetShaderResourceView(const Shader& s, unsigned slot, const View* view) {
        if (view) BindView(backend, lcue, HardwareStage(s.frequency, s.vertex_stage), slot, *view);
    }
    static unsigned LooseSlot(Frequency f) {
        switch (f) {
        case Frequency::Vertex: return 0;
        case Frequency::Pixel: return 1;
        case Frequency::Geometry: return 2;
        default: throw std::invalid_argument("hull/domain loose constants unsupported");
        }
    }
    void RHISetShaderParameter(const Shader& s, unsigned /*buffer_index*/, u32 offset,
                               u32 size, const void* data) {
        loose[LooseSlot(s.frequency)].Write(offset, size, data);
    }
    void PrepareGraphics(u32 next_first_instance) {
        Check(pipeline != nullptr, "graphics pipeline missing");
        vertex_dirty |= first_instance != next_first_instance;
        if (vertex_dirty) {
            for (const auto& e : pipeline->elements) {
                if (e.binding == 255) continue;
                Check(e.stream < streams.size(), "vertex stream selector");
                if (!streams[e.stream].address) continue;
                const auto descriptor = VertexDescriptor(streams[e.stream], e, next_first_instance);
                const auto stage = pipeline->vertex_stage;
                lcue.stages[Index(stage)].Bind(backend, stage, Binding::VertexBuffer,
                                               e.binding, descriptor.data());
            }
            vertex_dirty = false; first_instance = next_first_instance;
        }
        PrepareShader(pipeline->vertex, 1, pipeline->vertex_stage);
        PrepareShader(pipeline->pixel, 2, Stage::Pixel);
        PrepareShader(pipeline->geometry, 4, Stage::Geometry);
        discard_loose_extent = false;
    }
    void PrepareShader(const Shader* s, u8 loose_bit, Stage loose_stage) {
        if (!s) return;
        uniforms.Commit(backend, lcue, s->frequency, HardwareStage(s->frequency, s->vertex_stage), s->maps);
        if (pipeline->loose_mask & loose_bit) {
            auto& shadow = loose[LooseSlot(s->frequency)];
            Descriptor descriptor;
            if (shadow.Commit(backend, discard_loose_extent, descriptor)) {
                lcue.stages[Index(loose_stage)].Bind(backend, loose_stage,
                    Binding::ConstantBuffer, 0, descriptor.data());
                shadow.Acknowledge();
            }
        }
    }
    void CommitGraphics() { lcue.Commit(backend); }
    void BaseVertex(u32 value) { backend.BaseVertex(value); }
    void Instances(u32 value) { backend.Instances(value); }
    void IndexType(u32 type, u32 mode) { backend.IndexType(type, mode); }
    void DrawAuto(u32 count, u32 flags) { backend.DrawAuto(count, flags); }
    void DrawIndex(u32 count, u64 address, u32 flags) { backend.DrawIndex(count, address, flags); }
    void SetBase(u32 slot, u64 address) { backend.SetBase(slot, address); }
    void IndexBase(u64 address) { backend.IndexBase(address); }
    void IndexSize(u32 count) { backend.IndexSize(count); }
    void DrawIndirect(u32 offset, Stage stage, u32 lo, u32 hi, u32 flags) {
        backend.DrawIndirect(offset, stage, lo, hi, flags);
    }
    void DrawIndexIndirect(u32 offset, Stage stage, u32 lo, u32 hi, u32 flags) {
        backend.DrawIndexIndirect(offset, stage, lo, hi, flags);
    }
    void RHIDrawPrimitive(u32 base, u32 primitives, u32 instances) {
        Check(pipeline != nullptr, "graphics pipeline missing");
        DrawPrimitive(*this, draw_state, pipeline->topology, base, primitives, instances);
    }
    void RHIDrawIndexedPrimitive(const IndexBuffer& ib, u32 base, u32 first, u32 vertices,
                                 u32 start, u32 primitives, u32 instances) {
        Check(pipeline != nullptr, "graphics pipeline missing");
        DrawIndexedPrimitive(*this, draw_state, pipeline->topology, ib, base, first,
                              vertices, start, primitives, instances);
    }
    u8 IndirectSelectors() const {
        Check(pipeline != nullptr, "graphics pipeline missing"); ValidatePipeline(*pipeline);
        return pipeline->geometry ? pipeline->vertex->export_program.indirect_selectors
                                  : pipeline->vertex->program.indirect_selectors;
    }
    void RHIDrawPrimitiveIndirect(const ArgumentBuffer& args, u32 byte_offset) {
        const auto selectors = IndirectSelectors();
        DrawPrimitiveIndirect(*this, draw_state, args, byte_offset, selectors, pipeline->vertex_stage);
    }
    void RHIDrawIndexedIndirect(const IndexBuffer& ib, const ArgumentBuffer& args, u32 argument_index) {
        const auto selectors = IndirectSelectors();
        DrawIndexedIndirect(*this, draw_state, ib, args, IndexedArgumentOffset(argument_index), selectors, pipeline->vertex_stage);
    }
    void RHIDrawIndexedPrimitiveIndirect(const IndexBuffer& ib, const ArgumentBuffer& args, u32 byte_offset) {
        const auto selectors = IndirectSelectors();
        DrawIndexedIndirect(*this, draw_state, ib, args, byte_offset, selectors, pipeline->vertex_stage);
    }
};
}} // namespace Tetris426::Graphics
