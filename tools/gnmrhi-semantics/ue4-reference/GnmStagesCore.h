#pragma once
#include "GnmGraphicsCore.h"

// KH3 01.10 native stage algorithms. Normalized inputs, C++14, no SDK headers.
// Addresses/evidence and the missing UE HS/DS selection bridge are documented in
// docs/gnm/kh3-gnmrhi-stages.md. uint32_t arithmetic intentionally wraps.
namespace kh3_gfx
{
struct GeometryMetadata
{
    uint32_t ItemDwords[4] = {}; // native GS +24..+30, four output streams
    uint32_t Word34 = 0;
    uint8_t CopyFlags = 0; // GS + 4*byte(GS+3) + 0x5e == copy header +26
    uint32_t Vertices() const { return Word34 & 0x7ff; }
    bool OnChip() const { return (CopyFlags & 2) != 0; }
};
inline uint64_t CopyShaderAddress(uint64_t GS, uint8_t HeaderByte3)
{
    return GS + 4u * HeaderByte3 + 0x38;
}
inline Reflection CopyReflection(bool OnChip, bool Compact)
{
    // Four built-ins initialized by 0x4c3e390, relative LCUE offsets
    // c268 / c380 (off-chip), c498 / c5b0 (on-chip).
    // Compact means high byte of copy-header DWORD0 == 1, not one stream.
    Reflection R;
    R.TableDwords = Compact ? 0 : 64;
    R.Selector[OnChip ? 15 : 12] = 0;
    if (!Compact) { R.Selector[6] = 2; R.Offset20 = 0; }
    return R; // raw +10a..110 = 0,4,8,12: auxiliary map, not vertex map +ea
}

struct GeometryBudget { uint32_t Lds512Units = 0, Groups = 0; };
inline GeometryBudget OnChipBudget(const GeometryMetadata& G)
{
    const uint32_t Instances = ((G.Word34 >> 11) & 31) + 1;
    uint32_t Sum = 0; for (auto S : G.ItemDwords) Sum += S;
    const uint32_t Bytes = 4 * (Instances * ((G.Word34 >> 16) & 255) + G.Vertices() * Sum);
    GeometryBudget B;
    // Zero would divide by zero in the binary. Reject malformed metadata here.
    if (!Bytes || Bytes > 65536) return B;
    const uint32_t Limit = Instances == 32 ? 63 : 64;
    B.Groups = 65536 / Bytes;
    if (B.Groups > Limit) B.Groups = Limit;
    B.Lds512Units = (Bytes * B.Groups + 511) >> 9;
    return B;
}
inline uint32_t GeometryCut(const GeometryMetadata& G)
{
    if (G.OnChip()) return (G.CopyFlags >> 4) & 3;
    uint32_t Sum = 0; for (auto S : G.ItemDwords) Sum += S;
    const uint32_t Bytes = Sum * ((4 * uint16_t(G.Word34)) & 0x1ffc);
    return Bytes < 0x204 ? 3 : Bytes < 0x404 ? 2 : Bytes < 0x804 ? 1 : 0;
}
inline uint32_t PatchEsResource2(uint32_t NativeWord14, uint32_t Lds512Units)
{
    if (!Lds512Units) return NativeWord14; // zero selects the unmodified SDK body
    return (NativeWord14 & 0xe00fffff) | ((Lds512Units << 20) & 0x1ff00000);
}

template<class Sink> void ContextRegister(Sink& B, uint32_t Register, uint32_t Value)
{
    const uint32_t P[] = {0xc0016900, Register, Value}; B.Emit(P, 3);
}
template<class Sink> void ResetGeometryRegisters(Sink& B)
{
    // Constants 0x78abdc0 / 0x77dae30 and trailing qword 741. These are two
    // register writes, not a GPU fence or proof that the rings may be freed.
    const uint32_t P[] = {0xc0016900,0x290,0,0xc0016900,0x2e5,0}; B.Emit(P, 6);
}
struct GeometryMode
{
    uint32_t Mode = 0, Cut = 0, OnChipKey = ~0u, StageEnable = 0;
    template<class Sink> void Select(Sink& B, const GeometryMetadata& G, uint32_t Groups)
    {
        const uint32_t NextCut = GeometryCut(G);
        if (G.OnChip())
        {
            const uint32_t SubgroupVertices = Groups * ((uint16_t(G.Word34) >> 11) + 1);
            const uint32_t Key = (NextCut << 22) | Groups | ((SubgroupVertices << 11) & 0x7fff800);
            if (Mode != 0x600003 || Key != OnChipKey)
            {
                if (OnChipKey != ~0u) ResetGeometryRegisters(B);
                ContextRegister(B, 0x291, (SubgroupVertices & 0x7ff) | ((Groups << 11) & 0x3ff800));
                ContextRegister(B, 0x290, 0x600003 | (NextCut << 4));
            }
            Mode = 0x600003; OnChipKey = Key;
        }
        else
        {
            if (Mode != 0x180003 || Cut != NextCut)
                ContextRegister(B, 0x290, 0x180003 | (NextCut << 4));
            Mode = 0x180003;
        }
        Cut = NextCut;
        if (StageEnable != 0xb0) ContextRegister(B, 0x2d5, 0xb0);
        StageEnable = 0xb0;
    }
    template<class Sink> void Disable(Sink& B)
    {
        if (!StageEnable) return;
        if (Mode) ResetGeometryRegisters(B);
        Mode = Cut = StageEnable = 0; OnChipKey = ~0u;
        ContextRegister(B, 0x2d5, 0);
    }
};

// 0x68bbb10. Context register offsets are DWORD indices, not byte addresses.
template<class Sink> void GeometryRingRegisters(Sink& B, const GeometryMetadata& G)
{
    const uint32_t* S = G.ItemDwords; const uint32_t V = G.Vertices();
    const uint32_t Items[] = {0xc0046900,0x2d7,S[0]&0x7fff,S[1]&0x7fff,S[2]&0x7fff,S[3]&0x7fff};
    const uint32_t Offsets[] = {0xc0036900,0x298,(V*S[0])&0x7fff,
        (V*(S[0]+S[1]))&0x7fff,(V*(S[0]+S[1]+S[2]))&0x7fff};
    B.Emit(Items, 6); B.Emit(Offsets, 5);
    ContextRegister(B, 0x2ce, V);
    ContextRegister(B, 0x2ac, (V*(S[0]+S[1]+S[2]+S[3]))&0x7fff);
}
inline void BufferDescriptor(uint32_t* D, uint64_t Base, uint32_t HighFlags,
                             uint32_t Records, uint32_t Format)
{
    D[0] = uint32_t(Base); D[1] = (uint32_t(Base >> 32) & 0xfff) | HighFlags;
    D[2] = Records; D[3] = Format;
}
inline void GeometryRingDescriptors(uint32_t Table[48], uint64_t EsGs, uint64_t GsVs,
                                    const GeometryMetadata& G)
{
    // 0x382bf0 + 0x4c3ec80. Preserve slots 0/1 (auxiliary descriptors) and
    // DWORDs 36..47; the binary doesn't initialize that tail in this routine.
    BufferDescriptor(Table+8, EsGs, 0x80003000, 0x400000, 0xea7204);
    BufferDescriptor(Table+12, EsGs, 0x3000, 0x400000, 0x27204);
    uint64_t PrefixBytes = 0;
    for (uint32_t I = 0; I < 4; ++I)
    {
        const uint64_t Address = GsVs + (uint64_t(G.Vertices()) << 6) * PrefixBytes;
        BufferDescriptor(Table+16+4*I, Address,
            0x80003000 | (((G.Vertices()<<18)*G.ItemDwords[I]) & 0x3ffc0000), 64, 0xaa7204);
        PrefixBytes += uint32_t(4 * G.ItemDwords[I]);
    }
    BufferDescriptor(Table+32, GsVs, 0x3000, 0x400000, 0x27204);
}
struct RingLayoutKey
{
    uint32_t Words[6] = {}; bool Valid = false;
    bool Matches(uint32_t EsWord1c, const GeometryMetadata& G) const
    {
        if (!Valid || Words[0] != EsWord1c || Words[5] != G.Vertices()) return false;
        for (uint32_t I=0; I<4; ++I) if (Words[I+1] != G.ItemDwords[I]) return false;
        return true;
    }
    void Remember(uint32_t EsWord1c, const GeometryMetadata& G)
    {
        Words[0]=EsWord1c; Words[5]=G.Vertices();
        for (uint32_t I=0; I<4; ++I) Words[I+1]=G.ItemDwords[I]; Valid=true;
    }
};

// Tessellation SDK arm of 0x4c3a220. This is an executable replay of supplied
// LS/HS selection state, NOT a recovered KH3 RHICreate/SetBoundShaderState path.
struct TessellationMetadata
{
    uint32_t LsWord1c = 0;
    uint32_t HsWord24 = 0, HsWord28 = 0, HsWord2c = 0, HsWord30 = 0;
    uint32_t HsWord34 = 0, HsWord3c = 0, PatchGroupCount = 0;
    int32_t HsOverride = 0; // raw LCUE+c708: negative enables low2-bit override
};
inline void TessellationConstants(const TessellationMetadata& T, uint32_t W[9])
{
    const bool Separate = T.HsWord28 != 0 && T.HsWord30 != 0;
    W[0]=T.LsWord1c; W[1]=Separate ? T.HsWord30 : T.LsWord1c; W[2]=T.PatchGroupCount;
    W[3]=Separate ? T.HsWord24*T.LsWord1c*T.PatchGroupCount : 0;
    W[4]=16*T.HsWord2c;
    W[5]=T.PatchGroupCount*(T.LsWord1c*T.HsWord24 + T.HsWord28*T.HsWord30);
    W[6]=T.HsWord28*W[1]; W[7]=0x44000000; W[8]=T.HsWord3c;
}
inline uint32_t HullConfiguration(const TessellationMetadata& T)
{
    return uint8_t(T.PatchGroupCount) | ((T.HsWord24&63)<<8) | ((T.HsWord34&63)<<14);
}
inline uint32_t PatchHsTfParam(uint32_t NativeWord18, int32_t Override)
{
    // Native HS+18 is VGT_TF_PARAM, not SPI_SHADER_PGM_RSRC2_HS.
    return Override < 0 ? (NativeWord18&0xfff9ffff) | ((uint32_t(Override)&3)<<17) : NativeWord18;
}
inline uint32_t DomainHardwareStage(uint32_t StageMode) { return 2+((StageMode&0x20)>>4); }
struct StageView
{
    uint64_t Program = 0; uint32_t Modifier = 0;
    Reflection Ref; Extras Extra; Selection State;
    uint32_t* Staging = nullptr; size_t StagingCount = 0;
    uint16_t TessellationSlot = 0xffff; // reflection WORD +e8, not UB slot 0
};
struct TessellationCache
{
    bool Enabled = true, Valid = false;
    uint32_t Descriptor[4] = {};
};
template<class Sink>
void CommitLsHs(Sink& B, uint32_t StageMode, const TessellationMetadata& T,
    StageView& LS, StageView& HS, StageView& Domain, TessellationCache& Cache,
    const uint32_t NativeHsBody[7])
{
    assert(StageMode&1);
    if (Cache.Enabled)
    {
        if (LS.State.ShaderDirty || HS.State.ShaderDirty)
        {
            uint32_t Words[9]; TessellationConstants(T, Words);
            const uint64_t Address = B.UploadTable(Words, 9);
            // Preserve exact descriptor record count 3; do not replace with 9/36.
            BufferDescriptor(Cache.Descriptor, Address, 0x100000, 3, 0x20077fac);
            Cache.Valid = true;
        }
        assert(Cache.Valid); // initialized by a previous commit or captured state
        Bind(B,5,HS.TessellationSlot,Cache.Descriptor,4,HS.Staging,HS.StagingCount,HS.State);
        Bind(B,DomainHardwareStage(StageMode),Domain.TessellationSlot,Cache.Descriptor,4,
             Domain.Staging,Domain.StagingCount,Domain.State);
    }
    // Sink SetLs/SetHs is the SDK boundary (reserve 23 / 30 DWORDs); it also
    // handles optional code prefetch. Upload and binding above precede BOTH.
    if (LS.State.ShaderDirty) B.SetLs(LS.Program, LS.Modifier);
    if (HS.State.ShaderDirty)
    {
        uint32_t Body[7]; for (int I=0; I<7; ++I) Body[I]=NativeHsBody[I];
        Body[4]=PatchHsTfParam(Body[4],T.HsOverride);
        B.SetHs(HS.Program,Body,HullConfiguration(T),HS.State.UpdateAllowed);
    }
    if (LS.State.ResourcesDirty) CommitTable(B,6,LS.Ref,LS.Extra,LS.Staging,LS.StagingCount);
    if (HS.State.ResourcesDirty) CommitTable(B,5,HS.Ref,HS.Extra,HS.Staging,HS.StagingCount);
    LS.State.Acknowledge(false); HS.State.Acknowledge();
    // Domain is committed later in non-GS mode. With mode&0x20, the binary has
    // ALREADY passed the ES/GS block: preserve Domain.ResourcesDirty here.
}
}
