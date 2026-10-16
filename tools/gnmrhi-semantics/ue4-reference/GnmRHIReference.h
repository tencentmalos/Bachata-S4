#pragma once

// Reference reconstruction, not Epic/Sony source and not a complete RHI module.
// Semantic target: KH3 CUSA15072 01.10 / ++UE4+Release-4.17.
// Public override signatures checked against UE 4.18-GameWorks b50e6338.
// These are NORMALIZED UE objects, never binary overlays. See README.md.
#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RHIContext.h"
#include "DynamicRHI.h"

struct FGnmDescriptor
{
    uint32 Words[8] = {};
    uint32 NumWords = 0; // 4: buffer/sampler, 8: texture
};

enum class EGnmAllocationKind : uint8
{
    UniformTransient, UniformPersistent, LooseIn64KiBBlock,
    LooseLarge, LcueTable, VolatileBuffer
};

struct FGnmAllocation
{
    uint8* Cpu = nullptr;
    uint64 Gpu = 0;
    uint32 Capacity = 0;
    uint64 OwnerToken = 0; // Adapter-owned allocation / lifetime identity.
};

struct FGnmGeometryRings
{
    // RHI-lifetime backing, NOT per-draw allocations. 0x3a5640 allocates each
    // large ring as 4 MiB / 64 KiB aligned. Aux buffers are 16 KiB each.
    uint64 EsGs = 0, GsVs = 0;
    uint32 Auxiliary[2][4] = {};
    uint32 AuxiliaryTableSlot[2] = {0, 1}; // actual manager indices, defaults from 3a9470
};

enum class EGnmResourceKind : uint8 { Texture, ShaderResourceView, Sampler };
struct FGnmFlushRange { uint64 Base; uint32 Bytes; };

// Decoded metadata supplied by analysis or a real shader loader. This is NOT a
// claim to have recovered RHICreateComputeShader or the serialized shader format.
struct FGnmComputeReflection
{
    uint16 TableDwords = 0;
    TArray<uint16> ResourceSlots; // binary reflection +0x22
    TArray<uint16> SamplerSlots;  // +0xa2
    TArray<uint16> ConstantSlots; // +0xc2; slot 0 also receives loose constants
    uint8 PointerSgprs[5] = {255, 255, 255, 255, 255}; // +7..+b
    uint16 SubtableDwords[4] = {}; // +0x16/+0x1a/+0x1c/+0x1e
    bool bHasUnrecoveredExtras = true; // +c/+d/+e; require explicit classification
};

struct FGnmShaderResourceTable
{
    uint32 ResourceTableBits = 0; // binary shader +0x200
    TArray<uint32> TextureMap;   // +0x248; each map includes its own UB prefix
    TArray<uint32> SRVMap;       // +0x208
    TArray<uint32> SamplerMap;   // +0x218
};

class FGnmComputeShader : public FRHIComputeShader
{
public:
    uint64 NativeProgram = 0; // binary pointer at shader +0x1d8, opaque here
    uint16 ProgramWord6 = 0; // raw program +6, used by 0x36cbf7
    uint8 ProgramByte24 = 0; // low two bits contribute to dispatch flags
    bool bUsesLooseConstants = false; // shader +0x258
    FGnmComputeReflection Reflection; // embedded at binary shader +0xb0
    FGnmShaderResourceTable ResourceTable;
};

// Deliberately narrow, REQUIRED platform boundary. Implementations must reserve
// capacity before emitting, preserve referenced memory until GPU completion, and
// fail on unsupported cases. An empty implementation is not a working backend.
class IGnmReferencePlatform
{
public:
    virtual ~IGnmReferencePlatform() {}
    virtual FGnmAllocation Allocate(EGnmAllocationKind Kind, uint32 Bytes,
                                    uint32 Alignment) = 0;
    // Called when the CPU UB dies. Persistent backing is ticket-queued, NOT freed
    // immediately. Transient backing remains owned by the frame/block allocator.
    virtual void RetireUniform(const FGnmAllocation& Allocation, bool bTransient) = 0;
    virtual void EmitPacket(const uint32* Words, uint32 NumWords) = 0;
    virtual FGnmDescriptor ResolveDescriptor(EGnmResourceKind Kind,
                                             FRHIResource* Resource) = 0;
    // ResolveDescriptor(SRV) must consult the CURRENT backing for multibuffers.
    virtual void SetCsShaderWithModifier(uint64 NativeProgram) = 0;
    virtual void DispatchDirect(uint32 X, uint32 Y, uint32 Z, uint32 Flags) = 0;
    virtual void DispatchIndirect(uint32 ArgumentOffset, uint32 Flags) = 0;
    // Packet implementations are in ../cpp/gnm_rhi_semantic.cpp. Real label
    // embedding, rollover and SDK allocation policy still need an adapter.
    virtual void FlushPriorGraphics(TArray<FGnmFlushRange>& Ranges) = 0;
    virtual void FlushAfterCompute() = 0;
    virtual void FinishComputeBookkeeping() = 0; // unrecovered texture/state tail
    // Graphics SDK boundary: Stage 2=VS, 1=PS, 3=GS. Program is the native header;
    // SDK Set/Update receives Program+8. Null PS is an intentional unbind.
    virtual void SetGraphicsShader(uint32 Stage, uint64 Program, uint32 Modifier, bool bUpdate) = 0;
    // Copy Program+8 body, replace native DWORD+14 with PatchedWord14, then
    // SetEsShaderWithModifier (20 DWORD reservation). No ES Update form here.
    virtual void SetExportShader(uint64 Program, uint32 Modifier, uint32 PatchedWord14) = 0;
    virtual FGnmGeometryRings GetGeometryRings() = 0;
    // Reserve >=52 DWORDs in DCB; emit NOP [0xc0301000,0x68750000] followed by
    // 48 payload DWORDs at 4-byte alignment, advance DCB by 50 DWORDs. Return
    // the payload address (CPU/GPU), with original bytes available for tail
    // preservation. This is not an ordinary LCUE upload allocation.
    virtual FGnmAllocation EmbedGeometryRingTable() = 0;
    virtual void PrefetchGraphicsShader(uint64 Program) = 0;
    // Shared fixed state and pending PS resource handling in 0x382bf0. GS mode
    // and ring register transitions are implemented by GnmStagesCore.h now.
    virtual void ApplyGraphicsSelectionSideEffects(uint32 SharedWord, bool bPixelEnabled) = 0;
    virtual uint32 GetRawIndexType(FIndexBufferRHIParamRef Buffer) = 0;
    virtual uint64 ResolveCurrentIndexAddress(FIndexBufferRHIParamRef Buffer) = 0;
    virtual void DrawIndexAuto(uint32 Count, uint32 Flags) = 0;
    virtual void DrawIndex(uint32 Count, uint64 Address, uint32 Flags, uint32 FinalArgument) = 0;
};

// Own the layout before FRHIUniformBuffer takes its address. This ownership is a
// reference-code convenience; KH3 stores the caller's layout pointer at +0x18.
struct FGnmOwnedUniformLayout
{
    FRHIUniformBufferLayout OwnedLayout;
    explicit FGnmOwnedUniformLayout(const FRHIUniformBufferLayout& Layout)
        : OwnedLayout(FRHIUniformBufferLayout::Zero) { OwnedLayout.CopyFrom(Layout); }
};

class FGnmUniformBuffer : private FGnmOwnedUniformLayout, public FRHIUniformBuffer
{
public:
    FGnmUniformBuffer(IGnmReferencePlatform& Platform, const void* Contents,
                     const FRHIUniformBufferLayout& Layout, EUniformBufferUsage Usage);
    virtual ~FGnmUniformBuffer();
    FGnmDescriptor Constants;
    TArray<TRefCountPtr<FRHIResource>> Resources;

private:
    IGnmReferencePlatform& Platform;
    FGnmAllocation Allocation;
    bool bTransient;
};

class FGnmVertexBuffer : public FRHIVertexBuffer
{
public:
    // Caller supplies allocations and current index; creation/destruction of VB
    // allocations is outside this excerpt. No guessed initial ring position.
    FGnmVertexBuffer(uint32 Size, uint32 Usage, const TArray<FGnmAllocation>& Slots,
                     uint32 CurrentSlot);
    void* Lock(IGnmReferencePlatform& Platform, uint32 Offset, EResourceLockMode Mode);
    const FGnmAllocation& CurrentData() const;

private:
    TArray<FGnmAllocation> BackingSlots;
    uint32 CurrentSlot;
};

struct FGnmLooseConstants
{
    TArray<uint8> Shadow; // caller supplies initial bytes; not implicitly zero
    uint32 UpdatedEnd = 0;
    uint32 PriorExtent = 0;
    bool bDirty = false;
    void Write(uint16 Offset, uint16 NumBytes, const void* Source);
    bool Commit(IGnmReferencePlatform& Platform, bool bDiscardOldExtent,
                FGnmDescriptor& OutDescriptor);
    void AcknowledgeBinding() { bDirty = false; UpdatedEnd = 0; }
};

struct FGnmComputeBindings
{
    TArray<uint32> Staging; // explicit captured/initialized prefix, no zero padding
    bool bDirty = false;
    void Bind(IGnmReferencePlatform& Platform, uint16 Mapping, const FGnmDescriptor& Descriptor);
    void Commit(IGnmReferencePlatform& Platform, const FGnmComputeReflection& Reflection);
};

#include "GnmGraphicsTypes.h"

// Other IRHICommandContext methods remain pure virtual. This excerpt cannot be
// instantiated or registered as the whole PS4 backend without implementing them.
class FGnmCommandContext : public IRHICommandContext
{
public:
    FGnmCommandContext(IGnmReferencePlatform& Platform, FGnmUniformBuffer* DefaultUniform,
                      const TArray<uint8>& InitialLooseShadow,
                      const TArray<uint32>& InitialStaging);

    virtual void RHISetComputeShader(FComputeShaderRHIParamRef ComputeShader) override;
    virtual void RHISetShaderParameter(FComputeShaderRHIParamRef ComputeShader,
        uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue) override;
    virtual void RHISetShaderUniformBuffer(FComputeShaderRHIParamRef ComputeShader,
        uint32 BufferIndex, FUniformBufferRHIParamRef Buffer) override;
    virtual void RHIDispatchComputeShader(uint32 ThreadGroupCountX,
        uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ) override;
    virtual void RHIDispatchIndirectComputeShader(FVertexBufferRHIParamRef ArgumentBuffer,
        uint32 ArgumentOffset) override;

    virtual void RHISetBoundShaderState(FBoundShaderStateRHIParamRef BoundShaderState) override;
    virtual void RHISetStreamSource(uint32 StreamIndex, FVertexBufferRHIParamRef VertexBuffer,
        uint32 Stride, uint32 Offset) override;
    virtual void RHIDrawPrimitive(uint32 PrimitiveType, uint32 BaseVertexIndex,
        uint32 NumPrimitives, uint32 NumInstances) override;
    virtual void RHIDrawIndexedPrimitive(FIndexBufferRHIParamRef IndexBuffer, uint32 PrimitiveType,
        int32 BaseVertexIndex, uint32 FirstInstance, uint32 NumVertices, uint32 StartIndex,
        uint32 NumPrimitives, uint32 NumInstances) override;

    virtual void RHISetShaderParameter(FVertexShaderRHIParamRef Shader, uint32 BufferIndex,
        uint32 BaseIndex, uint32 NumBytes, const void* NewValue) override;
    virtual void RHISetShaderParameter(FPixelShaderRHIParamRef Shader, uint32 BufferIndex,
        uint32 BaseIndex, uint32 NumBytes, const void* NewValue) override;
    virtual void RHISetShaderParameter(FGeometryShaderRHIParamRef Shader, uint32 BufferIndex,
        uint32 BaseIndex, uint32 NumBytes, const void* NewValue) override;
    virtual void RHISetShaderUniformBuffer(FVertexShaderRHIParamRef Shader, uint32 BufferIndex,
        FUniformBufferRHIParamRef Buffer) override;
    virtual void RHISetShaderUniformBuffer(FPixelShaderRHIParamRef Shader, uint32 BufferIndex,
        FUniformBufferRHIParamRef Buffer) override;
    virtual void RHISetShaderUniformBuffer(FGeometryShaderRHIParamRef Shader, uint32 BufferIndex,
        FUniformBufferRHIParamRef Buffer) override;
    virtual void RHISetShaderTexture(FVertexShaderRHIParamRef Shader, uint32 TextureIndex,
        FTextureRHIParamRef Texture) override;
    virtual void RHISetShaderTexture(FPixelShaderRHIParamRef Shader, uint32 TextureIndex,
        FTextureRHIParamRef Texture) override;
    virtual void RHISetShaderTexture(FGeometryShaderRHIParamRef Shader, uint32 TextureIndex,
        FTextureRHIParamRef Texture) override;
    virtual void RHISetShaderSampler(FVertexShaderRHIParamRef Shader, uint32 SamplerIndex,
        FSamplerStateRHIParamRef Sampler) override;
    virtual void RHISetShaderSampler(FPixelShaderRHIParamRef Shader, uint32 SamplerIndex,
        FSamplerStateRHIParamRef Sampler) override;
    virtual void RHISetShaderSampler(FGeometryShaderRHIParamRef Shader, uint32 SamplerIndex,
        FSamplerStateRHIParamRef Sampler) override;
    virtual void RHISetShaderResourceViewParameter(FVertexShaderRHIParamRef Shader, uint32 SRVIndex,
        FShaderResourceViewRHIParamRef SRV) override;
    virtual void RHISetShaderResourceViewParameter(FPixelShaderRHIParamRef Shader, uint32 SRVIndex,
        FShaderResourceViewRHIParamRef SRV) override;
    virtual void RHISetShaderResourceViewParameter(FGeometryShaderRHIParamRef Shader, uint32 SRVIndex,
        FShaderResourceViewRHIParamRef SRV) override;

    // Supply captured shadows, staging prefixes, draw state and mode before use.
    // This is not a recovered constructor, shader loader or BSS factory.
    FGnmGraphicsState Graphics;

    // Analysis inputs for policies whose setters/defaults are not all recovered.
    bool bAutoFlush = false;
    bool bPostComputeFlush = false;
    TArray<FGnmFlushRange> PendingGraphicsRanges;
    TArray<uint64> TrackedEntries; // context +0x428, count +0x430; meaning unknown

private:
    void CommitComputeResourceTables();
    void CommitResourceMap(const TArray<uint32>& Map, uint32 Slot, EGnmResourceKind Kind);
    void PrepareCompute(const FGnmAllocation* IndirectBacking);
    void FinishCompute();
    void BindGraphics(uint32 Stage, uint16 Mapping, const FGnmDescriptor& Descriptor);
    void SetGraphicsUniform(uint32 Stage, uint32 Slot, FUniformBufferRHIParamRef Buffer);
    void SetGraphicsResource(uint32 Stage, uint32 Slot, EGnmResourceKind Kind, FRHIResource* Resource);
    void CommitGraphicsResourceTables(uint32 Stage);
    void PrepareGraphics(uint32 FirstInstance);
    void CommitGraphicsLcue();
    IGnmReferencePlatform& Platform;
    TRefCountPtr<FGnmComputeShader> ComputeShader;
    TRefCountPtr<FGnmUniformBuffer> DefaultUniform;
    // 32 is the mask domain for this reference, not a recovered binary array size.
    TRefCountPtr<FGnmUniformBuffer> ComputeUniformBuffers[32];
    uint32 ComputeUniformDirty = 0;
    FGnmLooseConstants ComputeLoose;
    FGnmComputeBindings ComputeBindings;
    bool bComputeShaderDirty = false;
    uint8 ActiveStageFlags = 0;
    uint8 DispatchMode = 0;
};

// Resource creation and Lock live on FDynamicRHI; command state lives above.
// Other required FDynamicRHI methods intentionally remain pure virtual.
class FGnmDynamicRHI : public FDynamicRHI
{
public:
    FGnmDynamicRHI(IGnmReferencePlatform& Platform, FGnmCommandContext& DefaultContext)
        : Platform(Platform), DefaultContext(DefaultContext) {}
    virtual const TCHAR* GetName() override { return TEXT("GnmRHI KH3 reference"); }
    virtual IRHICommandContext* RHIGetDefaultContext() override { return &DefaultContext; }
    virtual FUniformBufferRHIRef RHICreateUniformBuffer(const void* Contents,
        const FRHIUniformBufferLayout& Layout, EUniformBufferUsage Usage) override;
    virtual void* RHILockVertexBuffer(FVertexBufferRHIParamRef VertexBuffer,
        uint32 Offset, uint32 SizeRHI, EResourceLockMode LockMode) override;
    virtual void RHIUnlockVertexBuffer(FVertexBufferRHIParamRef VertexBuffer) override;

private:
    IGnmReferencePlatform& Platform;
    FGnmCommandContext& DefaultContext;
};
