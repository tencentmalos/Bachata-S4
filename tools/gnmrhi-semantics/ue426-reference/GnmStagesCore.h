// SPDX-License-Identifier: GPL-2.0-or-later
// Tetris 4.26 stage reconstruction, independently checked against its ELF.
// Not a UE/SDK ABI overlay. See docs/gnm/tetris-ue426-gnmrhi-stages.md.
#pragma once
#include "GnmGraphicsCore.h"

namespace Tetris426 { namespace Graphics {
struct GeometryMetadata {
    std::array<u32, 4> items{}; // GS native +24..+30
    u32 word34 = 0;
    u8 copy_flags = 0; // embedded copy header +26
    u32 Vertices() const { return word34 & 0x7ff; }
    bool OnChip() const { return (copy_flags & 2) != 0; }
};
inline u64 CopyShaderAddress(u64 gs, u8 header3) { return gs + 4u * header3 + 0x38; }
inline Reflection CopyReflection(bool on_chip, bool compact) {
    // 3c4d900: aa44/ab58/ac6c/ad80, stride 114 (KH3: 118).
    Reflection r; r.bytes.fill(255);
    r.SetWord(0, compact ? 0 : 64); r.bytes[2] = 0; r.bytes[3] = 2;
    r.bytes[on_chip ? 15 : 12] = 0;
    if (!compact) {
        r.bytes[6] = 2; r.SetWord(0x1c, 0);
        for (unsigned i = 0; i < 4; ++i) r.SetWord(0x106 + i * 2, u16(i * 4));
    }
    const u8 counts[] = {48,16,16,20,16};
    std::copy(counts, counts + 5, r.bytes.begin() + 0x10e);
    return r; // +106 auxiliary maps are NOT vertex-buffer maps at +e6
}
struct GeometryBudget { u32 lds512 = 0, groups = 0; };
inline GeometryBudget OnChipBudget(const GeometryMetadata& g) {
    u32 sum = 0; for (auto x : g.items) sum += x;
    const u32 instances = ((g.word34 >> 11) & 31) + 1;
    const u32 bytes = 4 * (instances * ((g.word34 >> 16) & 255) + g.Vertices() * sum);
    Check(bytes != 0, "zero GS budget divides by zero in native helper");
    if (bytes > 65536) return {}; // 3c4bad0 failure; factory does not test its bool
    const u32 groups = std::min(65536u / bytes, instances == 32 ? 63u : 64u);
    return {(bytes * groups + 511) >> 9, groups};
}
inline u32 GeometryCut(const GeometryMetadata& g) {
    if (g.OnChip()) return (g.copy_flags >> 4) & 3;
    u32 sum = 0; for (auto x : g.items) sum += x;
    const u32 bytes = sum * ((g.word34 << 2) & 0x1ffc);
    return bytes < 0x204 ? 3 : bytes < 0x404 ? 2 : bytes < 0x804 ? 1 : 0;
}
inline u32 PatchEsResource2(u32 original, u32 lds512) {
    return lds512 ? (original & 0xe00fffff) | ((lds512 << 20) & 0x1ff00000) : original;
}
template<class Sink> void ContextRegister(Sink& sink, u32 reg, u32 value) {
    const u32 p[] = {0xc0016900, reg, value}; sink.Emit(p, 3);
}
template<class Sink> void ResetGeometryRegisters(Sink& sink) {
    const u32 p[] = {0xc0016900,0x290,0,0xc0016900,0x2e5,0}; sink.Emit(p, 6);
}
struct GeometryMode {
    u32 mode = 0, cut = 0, key = ~0u;
    template<class Sink> void Select(Sink& sink, const GeometryMetadata& g, u32 groups) {
        const u32 next_cut = GeometryCut(g);
        if (g.OnChip()) {
            const u16 n = u16(groups * (((g.word34 >> 11) & 31) + 1));
            const u32 next_key = (next_cut << 22) | (u32(n) << 11) | groups;
            if (mode != 0x600003 || next_key != key) {
                if (key != ~0u) ResetGeometryRegisters(sink);
                ContextRegister(sink, 0x291, (n & 0x7ff) | ((groups << 11) & 0x3ff800));
                ContextRegister(sink, 0x290, 0x600003 | (next_cut << 4));
            }
            mode = 0x600003; key = next_key;
        } else {
            if (mode != 0x180003 || cut != next_cut)
                ContextRegister(sink, 0x290, 0x180003 | (next_cut << 4));
            mode = 0x180003; // native off-chip selector does NOT clear old on-chip key
        }
        cut = next_cut; // stage-enable belongs to UE apply, not this selector
    }
};
struct RingStorage {
    u64 esgs = 0, gsvs = 0;
    u32 esgs_bytes = 0x400000, gsvs_bytes = 0x400000;
    // fa1c50 initializes two 4 MiB / 4 MiB-aligned allocations via fec130.
    // Caller supplies actual addresses and owns allocation/retirement.
};
using RingTable = std::array<u32, 48>;
inline void FillRingTable(RingTable& table, const RingStorage& rings, const GeometryMetadata& g) {
    Check(rings.esgs && rings.gsvs, "ring backing missing");
    auto put = [&](unsigned slot, u64 address, u32 flags, u32 records, u32 format) {
        const Descriptor d{{u32(address), (u32(address >> 32) & 0xfff) | flags, records, format}};
        std::copy(d.begin(), d.end(), table.begin() + 4 * slot);
    };
    put(2, rings.esgs, 0x80003000, rings.esgs_bytes, 0xea7204);
    put(3, rings.esgs, 0x3000, rings.esgs_bytes, 0x27204);
    u64 prefix_bytes = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const u64 address = rings.gsvs + u64(g.Vertices() << 6) * prefix_bytes;
        put(4+i, address, 0x80003000 | ((g.Vertices() * g.items[i] << 18) & 0x3ffc0000),
            64, 0xaa7204);
        prefix_bytes += u32(g.items[i] << 2);
    }
    put(8, rings.gsvs, 0x3000, rings.gsvs_bytes, 0x27204);
    // Native 3c4bf30 is a plain shared-table store, with no dirty propagation.
    // Slots 0/1 and DWORD36..47 are caller-owned, not zeroed by this helper.
}
template<class Sink> void GeometryRingRegisters(Sink& sink, const GeometryMetadata& g) {
    const auto& s = g.items; const u32 v = g.Vertices();
    const u32 items[] = {0xc0046900,0x2d7,s[0]&0x7fff,s[1]&0x7fff,s[2]&0x7fff,s[3]&0x7fff};
    const u32 offsets[] = {0xc0036900,0x298,(v*s[0])&0x7fff,
        (v*(s[0]+s[1]))&0x7fff,(v*(s[0]+s[1]+s[2]))&0x7fff};
    sink.Emit(items, 6); sink.Emit(offsets, 5);
    ContextRegister(sink, 0x2ce, v); ContextRegister(sink, 0x2ac, (v*(s[0]+s[1]+s[2]+s[3]))&0x7fff);
}
inline bool SameRingLayout(u32 es1, const GeometryMetadata& a, u32 es2, const GeometryMetadata& b) {
    return es1 == es2 && a.items == b.items && a.Vertices() == b.Vertices();
}

struct TessellationMetadata {
    u32 ls1c = 0, hs24 = 0, hs28 = 0, hs2c = 0, hs30 = 0, hs34 = 0, hs3c = 0;
    u32 groups = 0, override_bits = 0;
};
inline std::array<u32, 9> TessellationConstants(const TessellationMetadata& t) {
    const bool separate = t.hs28 && t.hs30;
    const u32 stride = separate ? t.hs30 : t.ls1c;
    return {{t.ls1c, stride, t.groups, separate ? t.hs24*t.ls1c*t.groups : 0,
        16*t.hs2c, t.groups*(t.ls1c*t.hs24+t.hs28*t.hs30), t.hs28*stride, 0x44000000, t.hs3c}};
}
inline u32 HullConfig(const TessellationMetadata& t, u32 previous) {
    // 3f39b20 preserves high 12 bits. LCUE passes a stack word whose prior value
    // was not established in the inspected function. Replay supplies that value.
    return (previous & 0xfff00000) | u8(t.groups) | ((t.hs24&63)<<8) | ((t.hs34&63)<<14);
}
inline std::array<u32, 7> HullBody(const Program& p, u32 override_bits) {
    auto body = p.body;
    if (override_bits & 0x80000000) body[4] = (body[4]&0xfff9ffff) | ((override_bits&3)<<17);
    return body;
}

struct StageLcue : Lcue {
    std::array<Reflection, 4> copy_profiles{{CopyReflection(false,true), CopyReflection(false,false),
                                           CopyReflection(true,true), CopyReflection(true,false)}};
    GeometryMode geometry_mode;
    u32 es_modifier = 0, es_lds512 = 0, ls_modifier = 0;
    TessellationMetadata tess;
    Descriptor tess_descriptor{};
    bool tess_constants_enabled = true, tess_descriptor_valid = false;
    // Explicit replay input, not a guessed ABI default or observed zero.
    u32 hull_config_previous = 0;
    StageLcue() = default;
    StageLcue(const StageLcue&) = delete; // selected reflection pointers refer to own profiles
    StageLcue& operator=(const StageLcue&) = delete;

    template<class Sink> void SetMode(Sink& sink, u32 next, bool force = false) {
        Check(next==0 || next==0x45 || next==0xad || next==0xb0 || next==0x145 || next==0x1ad,
              "unrecognized native stage-enable mode");
        if (!force && mode == next) return;
        if (geometry_mode.mode && next!=0xad && next!=0xb0 && next!=0x1ad) {
            ResetGeometryRegisters(sink); geometry_mode = {};
            stages[3].selected.update = false;
        }
        mode = next; ContextRegister(sink, 0x2d5, next);
    }
    void SelectES(const Program& p, const Reflection& r, u64 fetch, u32 modifier, u32 lds512) {
        auto& s = stages[4].selected;
        s.resources_dirty = true; s.shader_dirty |= !s.program || s.program->identity != p.identity;
        s.program = &p; s.reflection = &r; // no Update test or emitted-bit clear in ES setter
        extras[4].fetch_shader = fetch; es_modifier = modifier; es_lds512 = lds512;
    }
    template<class Sink> void SelectGS(Sink& sink, const Program& p, const Reflection& r,
                                      const Program& copy, bool compact,
                                      const GeometryMetadata& g, u32 groups) {
        stages[3].selected.Select(&p, &r, true); // same three-key comparison as VS
        const auto& cr = copy_profiles[(g.OnChip() ? 2 : 0) + (compact ? 0 : 1)];
        SelectVS(copy, cr, 0, 0);
        geometry_mode.Select(sink, g, groups);
        if (g.OnChip() && r.bytes[15]!=255)
            extras[3].factor_a = extras[2].factor_a = u16(groups * (((g.word34 >> 11)&31)+1));
    }
    void SetExportFactor(u16 value) {
        Check(stages[3].selected.reflection, "GS reflection missing");
        if (stages[3].selected.reflection->bytes[15]!=255)
            extras[3].factor_b = extras[2].factor_b = value;
        stages[2].selected.resources_dirty = stages[3].selected.resources_dirty = true;
    }
    template<class Sink> void UploadRings(Sink& sink, RingTable table, const RingStorage& storage,
                                         u32 es_word1c, const GeometryMetadata& g) {
        FillRingTable(table, storage, g);
        const u64 address = sink.UploadTable(table.data(), unsigned(table.size()));
        for (auto& e : extras) e.shared_pointer = address;
        ContextRegister(sink, 0x2ab, es_word1c & 0x7fff);
        GeometryRingRegisters(sink, g);
    }
    template<class Sink> void PrefetchStage(Sink& sink, Stage stage) {
        const auto* p = stages[Index(stage)].selected.program;
        if (prefetch && p) sink.Prefetch(p->code_address, p->code_bytes);
    }
    void Clear(Stage stage, bool clear_update) {
        auto& s = stages[Index(stage)].selected;
        s.shader_dirty = s.resources_dirty = false;
        if (clear_update) s.update = false;
    }
    template<class Sink> void CommitGeometry(Sink& sink) {
        auto& es = stages[4].selected; auto& gs = stages[3].selected;
        Check(es.program && gs.program, "GS mode requires ES and GS");
        if (es.shader_dirty) {
            auto body = es.program->body; body[3] = PatchEsResource2(body[3], es_lds512);
            sink.ExportShader(*es.program, body, es_modifier); // SetES SDK boundary, 23 DWORD
            PrefetchStage(sink, Stage::Export);
        }
        if (gs.shader_dirty) {
            sink.Shader(Stage::Geometry, gs.program, gs.update, 0); gs.emitted = true;
            PrefetchStage(sink, Stage::Geometry);
        }
        for (Stage s : {Stage::Export, Stage::Geometry})
            if (stages[Index(s)].selected.resources_dirty)
                CommitTable(sink, s, stages[Index(s)], extras[Index(s)]);
        Clear(Stage::Export, false); Clear(Stage::Geometry, true);
    }
    template<class Sink> void BindTessDescriptor(Sink& sink, Stage stage) {
        auto& s = stages[Index(stage)];
        Check(s.selected.reflection, "tess reflection missing");
        const u16 map = s.selected.reflection->Word(0xe4); // internal CB19, not public CB0
        if (map != 0xffff) {
            const unsigned at = map & 0x3fff;
            if (map & 0x8000) UserData(sink, stage, u16(at), tess_descriptor.data(), 4, true);
            else {
                Check(at+4<=s.staging.size(), "tess map overflow");
                std::copy(tess_descriptor.begin(), tess_descriptor.end(), s.staging.begin()+at);
            }
        }
        s.selected.resources_dirty = true;
    }
    // Replay requires caller-supplied LS/HS selections and native Update bit.
    // No invented UE RHISetHullShader/RHISetDomainShader selection bridge.
    template<class Sink> void CommitTessellation(Sink& sink) {
        auto& ls = stages[6].selected; auto& hs = stages[5].selected;
        Check(ls.program && hs.program, "tess mode requires LS and HS replay state");
        if (tess_constants_enabled) {
            if (ls.shader_dirty || hs.shader_dirty) {
                const auto constants = TessellationConstants(tess);
                const u64 address = sink.UploadTable(constants.data(), unsigned(constants.size()));
                tess_descriptor = {{u32(address),(u32(address>>32)&0xfff)|0x100000,3,0x20077fac}};
                tess_descriptor_valid = true;
            }
            Check(tess_descriptor_valid, "missing cached tess descriptor");
            BindTessDescriptor(sink, Stage::Hull);
            BindTessDescriptor(sink, mode & 0x20 ? Stage::Export : Stage::Vertex);
        }
        if (ls.shader_dirty) {
            sink.Shader(Stage::Local, ls.program, false, ls_modifier); // SetLS: 23 DWORD
            PrefetchStage(sink, Stage::Local);
        }
        if (hs.shader_dirty) {
            sink.HullShader(*hs.program, HullBody(*hs.program,tess.override_bits),
                            HullConfig(tess,hull_config_previous), hs.update); // 30 DWORD
            hs.emitted = true; PrefetchStage(sink, Stage::Hull);
        }
        for (Stage s : {Stage::Local, Stage::Hull})
            if (stages[Index(s)].selected.resources_dirty)
                CommitTable(sink, s, stages[Index(s)], extras[Index(s)]);
        Clear(Stage::Local, false); Clear(Stage::Hull, true);
    }
    template<class Sink> void Commit(Sink& sink) {
        if (mode & 0x20) CommitGeometry(sink);
        if (mode & 1) CommitTessellation(sink);
        CommitVertexPixel(sink);
        // Combined mode may leave ES resources dirty: HS block runs AFTER ES.
        // Preserve this observable ordering; do not silently run ES twice.
    }
};
}} // namespace Tetris426::Graphics
