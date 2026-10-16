#pragma once
#include "GnmStagesCore.h"

// Included by GnmRHIReference.h after the shared UE types. These are normalized
// analysis objects, not binary overlays or a recovered serialized shader format.
struct FGnmGraphicsReflection
{
    kh3_gfx::Reflection Lcue;
    TArray<uint16> ResourceSlots, SamplerSlots, ConstantSlots, VertexSlots;
    // Raw reflection offsets: +22, +a2, +c2, +ea respectively.
};

struct FGnmGraphicsShaderData
{
    uint64 NativeProgram = 0;
    uint32 UpdateKey[8] = {}; // program DWORDs +18..+34; VS compares first three
    TArray<uint16> Semantics;
    // VS: count byte+25, WORD table at 4*(byte+3 + 10 + byte+24).
    // PS: count byte+38, WORD table at 4*(byte+3 + 15). byte+3 is NOT DWORD[3].
    FGnmGraphicsReflection Reflection;
    FGnmShaderResourceTable ResourceTable;
    uint32 HardwareStage = 2; // VS object +380; normal VS=2, ES=4 in GS route
};

class FGnmVertexShader : public FRHIVertexShader, public FGnmGraphicsShaderData
{
public:
    FGnmGraphicsShaderData AlternateES; // raw +260 native program / +268 reflection
    uint32 EsWord14 = 0, EsWord1c = 0; // raw native header DWORDs, supplied by loader
};
class FGnmGeometryShader : public FRHIGeometryShader, public FGnmGraphicsShaderData
{
public:
    FGnmGeometryShader() { HardwareStage = 3; }
    kh3_gfx::GeometryMetadata Metadata;
    FGnmGraphicsShaderData CopyShader; // native address = GS+4*byte(GS+3)+38
    bool bCompactCopy = false; // high byte of copy header DWORD0 == 1
    void FinishDecodedMetadata() // call once, before publishing the shader
    {
        CopyShader.Reflection.Lcue = kh3_gfx::CopyReflection(Metadata.OnChip(), bCompactCopy);
    }
};
class FGnmPixelShader : public FRHIPixelShader, public FGnmGraphicsShaderData
{
public:
    FGnmPixelShader() { HardwareStage = 1; }
};
class FGnmVertexDeclaration : public FRHIVertexDeclaration
{
public:
    FVertexDeclarationElementList Elements; // raw +14, count +94
};
class FGnmBoundShaderState : public FRHIBoundShaderState
{
public:
    TRefCountPtr<FGnmVertexDeclaration> Declaration;
    TRefCountPtr<FGnmVertexShader> VS;
    TRefCountPtr<FGnmPixelShader> PS;
    TRefCountPtr<FGnmGeometryShader> GS;
    TArray<int32> AttributeRemap; // raw +180 pointer, negative means skip
    uint64 FetchShader = 0; // raw +48
    uint32 Modifier = 0; // +190
    uint32 SharedWord = 0; // +1a0, raw state-register source
    bool bUsesVertexLoose = false, bUsesPixelLoose = false; // +1a4 / +1a7
    bool bUsesGeometryLoose = false; // +1a8
    bool bHasTessellation = false; // selection bridge not recovered; fail explicitly
};

struct FGnmGraphicsStage
{
    const FGnmGraphicsShaderData* Shader = nullptr; // owned by retained BSS
    kh3_gfx::Selection Selection;
    kh3_gfx::Extras Extras;
    TArray<uint32> Staging;
    TRefCountPtr<FGnmUniformBuffer> Uniforms[20]; // spacing 0xa0 + dirty mask 0xfffff
    uint32 UniformDirty = 0;
    FGnmLooseConstants Loose;
};

struct FGnmGraphicsState
{
    TRefCountPtr<FGnmBoundShaderState> Bound;
    FGnmGraphicsStage VS, PS, ES, GS;
    uint32 VertexStage = 2;
    kh3_gfx::GeometryMode GeometryMode;
    kh3_gfx::RingLayoutKey RingKey;
    uint32 EsLds512Units = 0;
    uint64 RingTableGpu = 0;
    kh3_gfx::Stream Streams[16]; // (+aa8 - +928) / 24
    kh3_gfx::DrawState Draw;
    uint32 FirstInstance = 0;
    bool bVertexDirty = false, bDiscardLooseExtent = false;
    bool bNeo = false, bPrefetch = false;
    uint32 DrawFlags = 0; // GNM+28 & 1; injected until its setter is recovered
    uint32 PendingPixelResourceCount = 0; // context+420, maintained by platform adapter
    // CPU retention ring from 0x382bf0. This is NOT the GPU retirement fence.
    TRefCountPtr<FGnmBoundShaderState> History[2500];
    uint32 HistoryCursor = 0;
};
