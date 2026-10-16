// SPDX-License-Identifier: GPL-2.0-or-later
// Tetris CUSA13427 Release-4.26, independently reconstructed from its x86-64 ELF.
// Normalized C++14 objects, NOT guest ABI overlays or Epic/Sony source.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace Tetris426 { namespace Graphics {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Descriptor = std::array<u32, 4>;
enum class Frequency : unsigned { Vertex, Hull, Domain, Pixel, Geometry };
enum class Stage : unsigned { Compute, Pixel, Vertex, Geometry, Export, Hull, Local };
inline unsigned Index(Stage s) { return static_cast<unsigned>(s); }
inline unsigned Index(Frequency f) { return static_cast<unsigned>(f); }
inline void Check(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
}

// The five-way switches at f96b70/f96f70/f97c30 use shader+28 frequency.
// Hull/domain call the game's unsupported-stage reporting path. Do not silently
// turn them into a working tessellation implementation just because LCUE has HS/LS.
inline Stage HardwareStage(Frequency f, Stage vertex_stage) {
    switch (f) {
    case Frequency::Vertex: return vertex_stage; // shader+394, VS2 or ES4
    case Frequency::Pixel: return Stage::Pixel;
    case Frequency::Geometry: return Stage::Geometry;
    default: throw std::invalid_argument("Tetris UE hull/domain setter is unsupported");
    }
}

// Actual serialized reflection offsets differ from KH3 by four bytes after +10.
// 3c4f6f0/7d0/8b0/970/a30/bb0 select WORD maps below.
enum class Binding { ReadBuffer, ReadTexture, ReadWriteBuffer, ReadWriteTexture,
                     Sampler, ConstantBuffer, VertexBuffer };
struct Reflection {
    std::array<u8, 0x114> bytes{}; // includes auxiliary maps +106 and counts +10e
    Reflection() {
        std::fill(bytes.begin() + 4, bytes.begin() + 16, u8(255));
        std::fill(bytes.begin() + 0x1e, bytes.end(), u8(255));
    }
    u16 Word(unsigned offset) const {
        Check(offset + 1 < bytes.size(), "reflection offset");
        return u16(bytes[offset]) | (u16(bytes[offset + 1]) << 8);
    }
    void SetWord(unsigned offset, u16 value) {
        Check(offset + 1 < bytes.size(), "reflection offset");
        bytes[offset] = u8(value); bytes[offset + 1] = u8(value >> 8);
    }
    u16 Map(Binding kind, unsigned slot) const {
        unsigned base = 0, count = 0;
        switch (kind) {
        case Binding::ReadBuffer: case Binding::ReadTexture: base = 0x1e; count = 48; break;
        case Binding::ReadWriteBuffer: case Binding::ReadWriteTexture: base = 0x7e; count = 16; break;
        case Binding::Sampler: base = 0x9e; count = 16; break;
        case Binding::ConstantBuffer: base = 0xbe; count = 20; break;
        case Binding::VertexBuffer: base = 0xe6; count = 16; break;
        }
        Check(slot < count, "reflection binding slot");
        return Word(base + 2 * slot);
    }
};

struct Program {
    u64 identity = 0; // native program pointer, not shader code address
    std::array<u32, 8> update_key{}; // native +18..+34
    u64 code_address = 0; // native DWORD(+8) << 8
    u32 code_bytes = 0; // native DWORD(+0) & 0x7fffff
    std::vector<u16> semantics; // native header tables; see documentation
    u8 indirect_selectors = 0; // ordinary VS byte +27 (low/high nibble)
    std::array<u32, 7> body{}; // native +8..+23; ES/HS temporary patch source
};
struct Selection {
    bool shader_dirty = false, resources_dirty = false, update = false, emitted = false;
    const Program* program = nullptr;
    const Reflection* reflection = nullptr;
    void Select(const Program* next, const Reflection* reflect, bool vertex) {
        Check(!vertex || next, "ordinary VS cannot be null");
        shader_dirty |= (program ? program->identity : 0) != (next ? next->identity : 0);
        resources_dirty = vertex || next != nullptr;
        update = next && program && shader_dirty && emitted;
        for (unsigned i = 0; update && i < (vertex ? 3u : 8u); ++i)
            update = program->update_key[i] == next->update_key[i];
        emitted = false; // reselecting before commit loses Update eligibility
        program = next; reflection = reflect;
    }
};

// Sink: Emit(words,count), UploadTable(words,count)->u64. UploadTable must copy
// immediately into a NEW allocation retained through GPU retirement. It must
// propagate capacity failure; returning an old staging pointer is incorrect.
template<class Sink>
void UserData(Sink& sink, Stage stage, u16 slot, const u32* words, unsigned count, bool marker) {
    static const u16 bases[] = {0x240, 0x0c, 0x4c, 0x8c, 0xcc, 0x10c, 0x14c};
    Check(Index(stage) < 7 && count && count <= 16, "user data bounds");
    u32 packet[20]; unsigned n = 0;
    if (marker) { packet[n++] = 0xc0001000; packet[n++] = 0x6875000d; }
    packet[n++] = 0xc0007600 | (count << 16) | (stage == Stage::Compute ? 2 : 0);
    packet[n++] = bases[Index(stage)] + slot;
    for (unsigned i = 0; i < count; ++i) packet[n++] = words[i];
    sink.Emit(packet, n);
}
struct StageBindings {
    // LCUE+90 + stage*1800; 0x1800 bytes per stage, seven hardware stages.
    std::array<u32, 0x600> staging{};
    Selection selected;
    template<class Sink>
    void Bind(Sink& sink, Stage stage, Binding kind, unsigned slot, const u32* words) {
        Check(selected.reflection != nullptr, "bind requires selected reflection");
        const u16 map = selected.reflection->Map(kind, slot);
        const unsigned count = (kind == Binding::ReadTexture || kind == Binding::ReadWriteTexture) ? 8 : 4;
        if (map != 0xffff) {
            const unsigned target = map & 0x3fff;
            if (map & 0x8000) UserData(sink, stage, u16(target), words, count, true);
            else {
                Check(target + count <= staging.size(), "staging map overflow");
                std::copy(words, words + count, staging.begin() + target);
            }
        }
        selected.resources_dirty = true; // even unused/direct mappings
    }
};
struct Extras {
    u64 shared_pointer = 0; // LCUE+78
    u64 fetch_shader = 0; // LCUE+a918
    u32 scalar_d = 0, scalar_e = 0;
    u16 factor_a = 0, factor_b = 0; // LCUE+aea4/+aea6, multiply for selector +f
};
template<class Sink>
void CommitTable(Sink& sink, Stage stage, const StageBindings& state, const Extras& extra) {
    Check(Index(stage) >= 1 && Index(stage) <= 6, "graphics table stage");
    Check(state.selected.reflection != nullptr, "table reflection missing");
    const Reflection& r = *state.selected.reflection;
    const unsigned count = r.Word(0);
    Check(count <= state.staging.size(), "table prefix too large");
    const u64 base = sink.UploadTable(state.staging.data(), count);
    auto pointer = [&](unsigned field, u64 address) {
        if (r.bytes[field] == 255) return;
        const u32 words[] = {u32(address), u32(address >> 32)};
        UserData(sink, stage, r.bytes[field], words, 2, false);
    };
    auto scalar = [&](unsigned field, u32 value) {
        if (r.bytes[field] != 255) UserData(sink, stage, r.bytes[field], &value, 1, true);
    };
    pointer(7, base);
    pointer(8, base + 4ull * r.Word(0x12));
    pointer(9, base + 4ull * r.Word(0x16));
    pointer(10, base + 4ull * r.Word(0x18));
    pointer(11, base + 4ull * r.Word(0x1a));
    pointer(12, extra.shared_pointer);
    scalar(13, extra.scalar_d); scalar(14, extra.scalar_e);
    if (stage == Stage::Vertex || stage == Stage::Export || stage == Stage::Local) {
        pointer(4, extra.fetch_shader);
        pointer(5, base + 4ull * r.Word(0x14));
        pointer(6, base + 4ull * r.Word(0x1c));
    }
    if (stage == Stage::Vertex || stage == Stage::Geometry)
        scalar(15, u32(extra.factor_a) * extra.factor_b);
}

// 3f39340, verified against both standard and Neo instruction branches.
inline u32 LinkPixelInput(u16 ps, const std::vector<u16>& vs, bool neo) {
    auto it = std::find_if(vs.begin(), vs.end(), [&](u16 v) { return u8(v) == u8(ps); });
    const bool found = it != vs.end();
    const u32 high = found ? *it >> 8 : 0;
    if (!neo || !(ps >> 14)) {
        const u32 v = found ? (high & 31) | ((ps >> 7) & 32) | ((ps | (ps >> 2)) & 0x400) : 32;
        return (v & 0x4ff) | (ps & 0x300);
    }
    const u32 offset = found ? high & 31 : 32;
    const u32 mode = found ? (ps >> 14) & ~(high >> 6) : 0;
    u32 value = 0x80000 | ((u32(ps) << 10) & 0x3000000);
    if (ps < 0xc000) {
        value |= (mode ? 32 : 0) | offset;
        return value | (((ps >> ((mode & 1) ? 8 : 12)) & 3) << 8);
    }
    value |= (offset | (32 * mode)) & 63;
    value |= (ps & 0x300) | ((mode & 2) << 19);
    return (value & 0x39fffff) | ((u32(ps) << 9) & 0x600000);
}

struct Lcue {
    std::array<StageBindings, 7> stages;
    std::array<Extras, 7> extras;
    u32 mode = 0, vs_modifier = 0;
    bool prefetch = false, neo = false;
    // A supplied native linkage table is used as-is; otherwise link semantic IDs.
    const std::vector<u32>* input_controls = nullptr;
    void SelectVS(const Program& p, const Reflection& r, u64 fetch, u32 modifier) {
        stages[2].selected.Select(&p, &r, true);
        extras[2].fetch_shader = fetch; vs_modifier = modifier; input_controls = nullptr;
    }
    void SelectPS(const Program* p, const Reflection* r) {
        stages[1].selected.Select(p, r, false); input_controls = nullptr;
    }
    // Sink additionally supplies Shader(stage, program, update, modifier),
    // Prefetch(address,bytes), PixelInputs(words,count). Shader is the actual SDK
    // Set/Update boundary (29 DWORD VS / 40 DWORD PS), not a fake completed draw.
    template<class Sink> void CommitOrdinary(Sink& sink) {
        Check((mode & 0x21) == 0, "GS/tess LCUE commit requires separate reconstruction");
        CommitVertexPixel(sink);
    }
    // Shared tail of 3c4de20, after GS and/or LS/HS branches.
    template<class Sink> void CommitVertexPixel(Sink& sink) {
        auto& vs = stages[2].selected; auto& ps = stages[1].selected;
        for (Stage s : {Stage::Vertex, Stage::Pixel}) {
            auto& selected = stages[Index(s)].selected;
            if (selected.shader_dirty) {
                sink.Shader(s, selected.program, selected.update, s == Stage::Vertex ? vs_modifier : 0);
                selected.emitted = true;
                if (prefetch && selected.program)
                    sink.Prefetch(selected.program->code_address, selected.program->code_bytes);
            }
        }
        for (Stage s : {Stage::Vertex, Stage::Pixel})
            if (stages[Index(s)].selected.resources_dirty)
                CommitTable(sink, s, stages[Index(s)], extras[Index(s)]);
        if ((vs.shader_dirty || ps.shader_dirty) && vs.program && ps.program && !ps.program->semantics.empty()) {
            if (input_controls) {
                Check(input_controls->size() >= ps.program->semantics.size(), "linkage table length");
                sink.PixelInputs(input_controls->data(), unsigned(ps.program->semantics.size()));
            } else {
                std::vector<u32> controls;
                for (u16 p : ps.program->semantics) controls.push_back(LinkPixelInput(p, vs.program->semantics, neo));
                sink.PixelInputs(controls.data(), unsigned(controls.size()));
            }
        }
        for (Stage s : {Stage::Vertex, Stage::Pixel}) {
            auto& selected = stages[Index(s)].selected;
            selected.shader_dirty = selected.resources_dirty = selected.update = false;
        }
    }
};

struct Stream { u64 address = 0; u32 remaining_bytes = 0; };
// f99210: stride is NOT an argument and is NOT cached in a stream.
inline Stream SnapshotStream(u64 current_address, u32 size, u32 offset, bool present = true) {
    return {present ? current_address + offset : 0, present ? size - offset : 0};
}
// Pipeline+ a0 array, 12 bytes/entry: native format, stride, stream, offset,
// binding selector, instancing byte, then two bytes not consumed by Prepare.
struct VertexElement {
    u32 format = 0;
    u16 stride = 0;
    u8 stream = 0, offset = 0, binding = 255, per_instance = 0;
};
inline u32 FormatBytes(u32 format) {
    static const u32 bits[61] = {
        0,8,16,16,32,32,32,32,32,32,32,64,64,96,128,0xffffffff,
        16,16,16,16,32,32,64,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
        0xffffffff,0xffffffff,0xffffffff,0xffffffff,16,16,32,4,8,8,4,8,8,8,
        0xffffffff,0xffffffff,8,8,8,8,8,8,16,16,32,32,32,64,64,8,16,1,1};
    const u8 index = u8(format);
    return index <= 0x3c ? bits[index] >> 3 : 0; // native 3f33270, no invented fallback
}
inline Descriptor VertexDescriptor(const Stream& stream, const VertexElement& e, u32 first_instance) {
    // IMUL is 32-bit then MOVSXD: a large instance product is SIGN-extended.
    const u32 product = first_instance * u32(e.stride);
    const std::int64_t signed_product = product & 0x80000000u ? std::int64_t(product) - 0x100000000ll : product;
    const u64 address = stream.address + e.offset + (e.per_instance ? u64(signed_product) : 0);
    const u32 records = e.stride ? stream.remaining_bytes / e.stride : FormatBytes(e.format);
    u32 extra = 0;
    if (!(e.format & 0x800) && u32(u8(e.format)) - 1 <= 13)
        if ((e.format & 0xf00) == 0x700 || (e.format & 0xfe) != 6) extra = (e.format & 15) << 15;
    return {{u32(address), (u32(address >> 32) & 0xfff) | ((u32(e.stride) << 16) & 0x3fff0000),
             records, ((e.format << 4) & 0x7000) | ((e.format >> 12) & 0xfff) | extra | 0x20000000}};
}

// f96a20 switches on the unified view's native kind (+18), using its descriptor
// at +28. It does not refresh a multibuffer address here as KH3's SRV path did.
struct View { unsigned kind = 0; std::array<u32, 8> words{}; };
template<class Sink>
void BindView(Sink& sink, Lcue& lcue, Stage stage, unsigned slot, const View& view) {
    static const Binding types[] = {Binding::ReadWriteTexture, Binding::ReadTexture,
        Binding::ReadWriteBuffer, Binding::ReadBuffer, Binding::ReadWriteBuffer, Binding::ReadBuffer};
    if (view.kind < 6) lcue.stages[Index(stage)].Bind(sink, stage, types[view.kind], slot, view.words.data());
}
struct Resource {
    enum class Kind { Sampler, Texture, ShaderView } kind = Kind::Sampler;
    std::array<u32, 8> words{};
    View view;
};
struct UniformBuffer {
    Descriptor descriptor{}; // native UB+58
    std::vector<const Resource*> resources; // native UB+28
};
struct ResourceMaps {
    u32 used_uniforms = 0; // shader+208
    std::vector<u32> samplers, textures, srvs, uavs; // +220/+250/+210/+230
};
template<class F> void WalkMap(const std::vector<u32>& map, unsigned ub, F visit) {
    Check(ub < map.size(), "resource map prefix missing");
    std::size_t at = map[ub];
    if (!at) return;
    Check(at < map.size() && map[at] >> 24 == ub, "resource map start invalid");
    for (;;) {
        Check(at < map.size(), "resource map sentinel missing");
        const u32 packed = map[at++];
        if ((packed >> 24) != ub) return;
        visit((packed >> 8) & 0xffff, packed & 255);
    }
}
struct UniformBindings {
    std::array<std::array<const UniformBuffer*, 20>, 5> buffers{};
    std::array<u32, 5> dirty{};
    template<class Sink>
    void Set(Sink& sink, Lcue& lcue, Frequency freq, Stage stage, unsigned slot, const UniformBuffer& ub) {
        Check(Index(freq) < 5 && slot < 20, "uniform slot");
        lcue.stages[Index(stage)].Bind(sink, stage, Binding::ConstantBuffer, slot, ub.descriptor.data());
        buffers[Index(freq)][slot] = &ub; dirty[Index(freq)] |= 1u << slot;
    }
    void PipelineChanged() { dirty.fill(0xfffff); }
    // Sink.TouchTexture(resource) implements last-use bookkeeping/lifetime. All
    // resource objects and descriptor allocations must remain valid through use.
    template<class Sink>
    void Commit(Sink& sink, Lcue& lcue, Frequency freq, Stage stage, const ResourceMaps& maps) {
        u32 pending = dirty[Index(freq)] & maps.used_uniforms;
        while (pending) {
            unsigned ub = 0; while (!(pending & (1u << ub))) ++ub;
            pending &= ~(1u << ub);
            Check(ub < 20 && buffers[Index(freq)][ub], "missing resource-bearing UB");
            const auto& resources = buffers[Index(freq)][ub]->resources;
            auto walk = [&](const std::vector<u32>& map, Resource::Kind kind, Binding binding) {
                WalkMap(map, ub, [&](unsigned index, unsigned slot) {
                    Check(index < resources.size() && resources[index], "missing UB resource");
                    const Resource& resource = *resources[index];
                    Check(resource.kind == kind, "UB resource kind");
                    if (kind == Resource::Kind::ShaderView) BindView(sink, lcue, stage, slot, resource.view);
                    else {
                        if (kind == Resource::Kind::Texture) sink.TouchTexture(resource);
                        lcue.stages[Index(stage)].Bind(sink, stage, binding, slot, resource.words.data());
                    }
                });
            };
            walk(maps.samplers, Resource::Kind::Sampler, Binding::Sampler);
            walk(maps.textures, Resource::Kind::Texture, Binding::ReadTexture);
            walk(maps.srvs, Resource::Kind::ShaderView, Binding::ReadBuffer);
            if (Index(stage) <= 1) walk(maps.uavs, Resource::Kind::ShaderView, Binding::ReadWriteBuffer);
        }
        dirty[Index(freq)] = 0; // entire UE-stage mask, including unused bits
    }
};

struct LooseConstants {
    std::vector<u8> shadow;
    u32 end = 0, previous_extent = 0;
    bool dirty = false;
    explicit LooseConstants(unsigned capacity = 4096) : shadow(capacity) {}
    // f97130 truncates offset/size arguments to 16 bits before copying.
    void Write(u32 offset_arg, u32 size_arg, const void* data) {
        const unsigned offset = u16(offset_arg), size = u16(size_arg);
        Check(offset + size <= shadow.size(), "loose constant range");
        if (size) std::memcpy(shadow.data() + offset, data, size);
        end = std::max(end, u32(offset + size)); dirty = true;
    }
    // f89150: only the dirty case allocates; clamp aligned extent to capacity.
    // Sink.UploadConstants copies bytes into retained command-buffer data storage.
    template<class Sink> bool Commit(Sink& sink, bool discard_old_extent, Descriptor& descriptor) {
        if (!dirty) return false;
        u32 size = discard_old_extent ? end : std::max(end, previous_extent);
        size = std::min(u32(shadow.size()), (size + 15) & ~15u);
        previous_extent = size;
        const u64 address = sink.UploadConstants(shadow.data(), size);
        descriptor = {{u32(address), (u32(address >> 32) & 0xfff) | 0x100000,
                       (size + 15) >> 4, 0x20077fac}};
        // Original caller clears +20 and +30 after binding, not the upload helper.
        return true;
    }
    void Acknowledge() { dirty = false; end = 0; }
};

inline u32 ElementCount(u32 topology, u32 primitives) {
    const u32 multiplier = topology == 0 || topology == 5 ? 3 : topology == 2 ? 2 : topology > 5 ? topology - 5 : 1;
    return primitives * multiplier + (topology == 1 ? 2 : 0);
}
struct DrawState { u32 index_type = 0, base_vertex = 0, instances = 1; };
struct IndexBuffer {
    u64 current_address = 0; u32 type = 0;
    u32 size = 0, stride = 2; // native +18 / +14, used for indirect IndexSize
};
struct ArgumentBuffer { u64 current_address = 0; };
// Sink.PrepareGraphics(first_instance), CommitGraphics(), BaseVertex, Instances,
// IndexType(type,2), DrawAuto(count,0), DrawIndex(count,address,0).
template<class Sink>
void DrawPrimitive(Sink& sink, DrawState& state, u32 pipeline_topology, u32 base, u32 primitives, u32 instances) {
    sink.PrepareGraphics(0);
    if (state.base_vertex != base) { sink.BaseVertex(base); state.base_vertex = base; }
    if (state.instances != instances) { sink.Instances(instances); state.instances = instances; }
    if (state.index_type != 0) { sink.IndexType(0, 2); state.index_type = 0; }
    sink.CommitGraphics(); sink.DrawAuto(ElementCount(pipeline_topology, primitives), 0);
    if (state.instances != 1) { sink.Instances(1); state.instances = 1; }
    if (state.base_vertex != 0) { sink.BaseVertex(0); state.base_vertex = 0; }
}
template<class Sink>
void DrawIndexedPrimitive(Sink& sink, DrawState& state, u32 topology, const IndexBuffer& ib,
                          u32 base, u32 first_instance, u32 /*num_vertices*/, u32 start_index,
                          u32 primitives, u32 instances) {
    // Byte offset arithmetic is 32-bit in f96460, then added to current backing.
    const u32 byte_offset = start_index * (ib.type ? 4u : 2u);
    sink.PrepareGraphics(first_instance);
    if (state.instances != instances) { sink.Instances(instances); state.instances = instances; }
    if (state.index_type != ib.type) { sink.IndexType(ib.type, 2); state.index_type = ib.type; }
    if (state.base_vertex != base) { sink.BaseVertex(base); state.base_vertex = base; }
    const u64 address = ib.current_address + byte_offset; // f965f4, BEFORE LCUE
    sink.CommitGraphics(); sink.DrawIndex(ElementCount(topology, primitives), address, 0);
    if (state.instances != 1) { sink.Instances(1); state.instances = 1; }
    if (state.base_vertex != 0) { sink.BaseVertex(0); state.base_vertex = 0; }
}

// f96310 / f96690 / f96840: selectors are VS+27 or ES/LS+19, supplied by caller.
template<class Sink> void FinishIndirect(Sink& sink, DrawState& state) {
    if (state.instances != 1) { sink.Instances(1); state.instances = 1; }
    sink.Instances(1); // actual unconditional tail call, even after the above
}
template<class Sink>
void DrawPrimitiveIndirect(Sink& sink, DrawState& state, const ArgumentBuffer& args,
                           u32 offset, u8 selectors, Stage stage = Stage::Vertex) {
    sink.PrepareGraphics(0);
    sink.SetBase(0, args.current_address);
    sink.CommitGraphics();
    sink.DrawIndirect(offset, stage, selectors & 15, selectors >> 4, 0);
    FinishIndirect(sink, state);
}
template<class Sink>
void DrawIndexedIndirect(Sink& sink, DrawState& state, const IndexBuffer& ib,
                         const ArgumentBuffer& args, u32 offset, u8 selectors, Stage stage = Stage::Vertex) {
    sink.PrepareGraphics(0);
    sink.SetBase(0, args.current_address);
    if (state.index_type != ib.type) { sink.IndexType(ib.type, 2); state.index_type = ib.type; }
    sink.IndexBase(ib.current_address);
    Check(ib.stride != 0, "indirect index stride");
    sink.IndexSize(ib.size / ib.stride);
    sink.CommitGraphics();
    sink.DrawIndexIndirect(offset, stage, selectors & 15, selectors >> 4, 0);
    FinishIndirect(sink, state);
}
inline u32 IndexedArgumentOffset(u32 argument_index) { return argument_index * 20u; }
}} // namespace Tetris426::Graphics
