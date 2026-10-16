#pragma once

// Independently written from KH3 01.10 pseudocode + x86-64. C++14, no UE/SDK
// substitutes. Shared by the UE adapter and executable algorithm/packet tests.
#include <cstdint>
#include <cstddef>
#include <cassert>

namespace kh3_gfx
{
struct Stream
{
    uint64_t Base = 0;
    uint32_t RemainingBytes = 0, Stride = 0, Records = 0;
};

inline Stream SnapshotStream(uint64_t Base, uint32_t Size, uint32_t Stride, uint32_t Offset)
{
    assert(!Base || Offset <= Size);
    Stream S;
    S.Base = Base ? Base + Offset : 0;
    S.RemainingBytes = Base ? Size - Offset : 0;
    S.Stride = Stride;
    S.Records = Stride ? S.RemainingBytes / Stride : S.RemainingBytes;
    return S;
}

// Exact 19-entry table at eboot+0x7eb9600, indexed by UE vertex element Type-1.
inline uint32_t VertexFormat(uint8_t Type)
{
    static const uint32_t Codes[] = {
        2115332, 2279179, 3852045, 16434958, 16433162, 16434186, 16433162,
        15917066, 181509, 16434444, 2277637, 2279173, 16434956, 16433420,
        181253, 16434188, 2277381, 16433164, 16433161};
    return Type >= 1 && Type <= 19 ? Codes[Type - 1] : 0;
}

inline void VertexDescriptor(const Stream& S, uint8_t Offset, uint8_t Type,
    uint16_t ElementStride, bool PerInstance, uint32_t FirstInstance, uint32_t Out[4])
{
    const uint32_t F = VertexFormat(Type);
    uint32_t Extra = 0;
    if (!(F & 0x800) && uint32_t(uint8_t(F)) - 1 <= 13)
        if ((F & 0xf00) == 0x700 || (F & 0xfe) != 6) Extra = (F & 15) << 15;
    // The multiply is 32-bit in the original, even though address addition is 64-bit.
    const uint32_t InstanceOffset = PerInstance ? FirstInstance * ElementStride : 0;
    const uint64_t Address = S.Base + Offset + InstanceOffset;
    Out[0] = uint32_t(Address);
    Out[1] = uint32_t(Address >> 32) & 0xfff;
    Out[1] |= (S.Stride << 16) & 0x3fff0000;
    Out[2] = S.Records; // no guessed subtraction of attribute / instance offset
    Out[3] = ((16 * F) & 0x7000) | Extra | ((F >> 12) & 0xfff) | 0x20000000;
}

// Normal path of 0x4c3a220; semantic IDs are the low bytes of packed WORDs.
inline uint32_t LinkPixelInput(uint16_t PS, const uint16_t* VS, size_t Count, bool Neo)
{
    size_t I = 0;
    while (I < Count && uint8_t(VS[I]) != uint8_t(PS)) ++I;
    const bool Found = I != Count;
    const uint32_t Hi = Found ? VS[I] >> 8 : 0;
    if (!Neo || !(PS >> 14))
    {
        uint32_t V = Found ? (Hi & 31) | ((PS >> 7) & 32) | ((PS | (PS >> 2)) & 0x400) : 32;
        return (V & ~0x300u) | (PS & 0x300);
    }
    const uint32_t Offset = Found ? Hi & 31 : 32;
    const uint32_t Mode = Found ? (PS >> 14) & ~(Hi >> 6) : 0;
    uint32_t V = 0x80000 | ((uint32_t(PS) << 10) & 0x3000000);
    if (PS < 0xc000)
    {
        V |= (Mode ? 32 : 0) | (Offset & 63);
        return V | (((PS >> (((4 * Mode) & 4) ^ 12)) & 3) << 8);
    }
    V |= (Offset | (32 * Mode)) & 63;
    V |= (PS & 0x300) | ((Mode & 2) << 19);
    return (V & 0xff9fffff) | ((uint32_t(PS) << 9) & 0x600000);
}

struct Selection
{
    bool ShaderDirty = false, ResourcesDirty = false, UpdateAllowed = false;
    bool Emitted = false;
    void Select(uint64_t Old, uint64_t Next, const uint32_t* OldKey,
                const uint32_t* NextKey, size_t KeyWords)
    {
        ShaderDirty |= Old != Next;
        ResourcesDirty = Next != 0;
        UpdateAllowed = Old && Next && ShaderDirty && Emitted;
        for (size_t I = 0; UpdateAllowed && I < KeyWords; ++I)
            UpdateAllowed = OldKey[I] == NextKey[I];
        Emitted = false; // selecting twice without emitting loses update eligibility
    }
    void Acknowledge(bool HasUpdateForm = true)
    {
        if (ShaderDirty && HasUpdateForm) Emitted = true;
        ShaderDirty = ResourcesDirty = UpdateAllowed = false;
    }
};

// Normalized reflection fields. Unknown/reserved bytes are not interpreted.
struct Reflection
{
    uint16_t TableDwords = 0;
    uint8_t Selector[16]; // byte offsets +4..+f are consumed below
    uint16_t Offset16 = 0, Offset18 = 0, Offset1a = 0, Offset1c = 0, Offset1e = 0, Offset20 = 0;
    Reflection() { for (auto& S : Selector) S = 255; }
};
struct Extras
{
    uint64_t SharedPointer = 0, FetchShader = 0; // LCUE+78 / +c130
    uint32_t ScalarD = 0, ScalarE = 0, ScalarF = 0; // raw values, no invented names
};

// Sink provides Emit(words,count), UploadTable(words,count)->GPU address.
// It must fail on capacity errors and retain every emitted allocation to retirement.
template<class Sink>
void UserData(Sink& B, uint32_t Stage, uint16_t Index, const uint32_t* Words,
              uint32_t Count, bool Marker)
{
    static const uint16_t Bases[] = {0x240, 0xc, 0x4c, 0x8c, 0xcc, 0x10c, 0x14c};
    assert(Stage < 7 && Count && Count <= 8);
    uint32_t P[12], N = 0;
    if (Marker) { P[N++] = 0xc0001000; P[N++] = 0x6875000d; }
    P[N++] = 0xc0007600 | (Count << 16) | (Stage == 0 ? 2 : 0);
    P[N++] = Bases[Stage] + Index;
    for (uint32_t I = 0; I < Count; ++I) P[N++] = Words[I];
    B.Emit(P, N);
}

template<class Sink>
void Bind(Sink& B, uint32_t Stage, uint16_t Map, const uint32_t* Words, uint32_t Count,
          uint32_t* Staging, size_t StagingCount, Selection& State)
{
    assert(Count == 4 || Count == 8);
    if (Map != 0xffff)
    {
        const uint16_t Index = Map & 0x3fff;
        if (Map & 0x8000) UserData(B, Stage, Index, Words, Count, true);
        else
        {
            assert(Index + Count <= StagingCount);
            for (uint32_t I = 0; I < Count; ++I) Staging[Index + I] = Words[I];
        }
    }
    State.ResourcesDirty = true; // even unused / direct mappings
}

template<class Sink>
void CommitTable(Sink& B, uint32_t Stage, const Reflection& R, const Extras& E,
                 const uint32_t* Staging, size_t StagingCount)
{
    assert(Stage >= 1 && Stage <= 6 && R.TableDwords <= StagingCount);
    const uint64_t Base = B.UploadTable(Staging, R.TableDwords);
    auto Pointer = [&](uint32_t Field, uint64_t Address) {
        if (R.Selector[Field] == 255) return;
        const uint32_t P[] = {uint32_t(Address), uint32_t(Address >> 32)};
        UserData(B, Stage, R.Selector[Field], P, 2, false);
    };
    auto Scalar = [&](uint32_t Field, uint32_t Value) {
        if (R.Selector[Field] != 255) UserData(B, Stage, R.Selector[Field], &Value, 1, true);
    };
    Pointer(7, Base);
    Pointer(8, Base + 4u * R.Offset16);
    Pointer(9, Base + 4u * R.Offset1a);
    Pointer(10, Base + 4u * R.Offset1c);
    Pointer(11, Base + 4u * R.Offset1e);
    Pointer(12, E.SharedPointer);
    Scalar(13, E.ScalarD);
    Scalar(14, E.ScalarE);
    if (Stage == 2 || Stage == 4 || Stage == 6)
    {
        Pointer(4, E.FetchShader);
        Pointer(5, Base + 4u * R.Offset18);
        Pointer(6, Base + 4u * R.Offset20);
    }
    if (Stage == 2 || Stage == 3) Scalar(15, E.ScalarF);
}

// Count helper accepts more UE topologies; the recovered direct GNM conversion
// only closes the triangle-list / triangle-strip / line-list intersection.
inline bool DrawShape(uint32_t Type, uint32_t Primitives, uint32_t& Topology, uint32_t& Count)
{
    if (Type == 0) { Topology = 4; Count = 3 * Primitives; }
    else if (Type == 1) { Topology = 6; Count = Primitives + 2; }
    else if (Type == 2) { Topology = 2; Count = 2 * Primitives; }
    else return false;
    return true;
}

struct DrawState
{
    // Supplied initial/captured state, not a recovered context constructor.
    uint32_t Topology = ~0u, IndexType = ~0u, BaseVertex = 0, Instances = 1;
    template<class Sink> void SetTopology(Sink& B, uint32_t Value, bool Neo)
    {
        if (Topology == Value) return;
        const uint32_t P[] = {0xc0017900, Neo ? 0x10000242u : 0x242u, Value};
        B.Emit(P, 3); Topology = Value;
    }
    template<class Sink> void SetBase(Sink& B, uint32_t Value)
    {
        if (BaseVertex == Value) return;
        const uint32_t P[] = {0xc0016800, 0x8102, Value};
        B.Emit(P, 3); BaseVertex = Value;
    }
    template<class Sink> void SetInstances(Sink& B, uint32_t Value)
    {
        if (Instances == Value) return;
        const uint32_t P[] = {0xc0002f00, Value};
        B.Emit(P, 2); Instances = Value;
    }
    template<class Sink> void SetIndexType(Sink& B, uint32_t Value)
    {
        if (IndexType == Value) return;
        const uint32_t P[] = {0xc0002a00, Value & 0xfffffb3f};
        B.Emit(P, 2); IndexType = Value;
    }
    template<class Sink> void AfterDraw(Sink& B) { SetInstances(B, 1); SetBase(B, 0); }
};
}
