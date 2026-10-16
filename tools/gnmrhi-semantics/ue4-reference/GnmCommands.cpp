#include "GnmRHIReference.h"

namespace
{
// GNM CS hardware stage is 0. UE SF_Compute is 5: never use it as this index.
void SetComputeUserData(IGnmReferencePlatform& Platform, uint16 Index,
                        const uint32* Words, uint32 Count, bool bDescriptorMarker)
{
    check(Count && Count <= 8);
    uint32 Packet[12];
    uint32 N = 0;
    if (bDescriptorMarker)
    {
        Packet[N++] = 0xc0001000;
        Packet[N++] = 0x6875000d;
    }
    Packet[N++] = 0xc0007602 | (Count << 16);
    Packet[N++] = 0x240 + Index;
    FMemory::Memcpy(Packet + N, Words, Count * sizeof(uint32));
    Platform.EmitPacket(Packet, N + Count);
}
}

void FGnmComputeBindings::Bind(IGnmReferencePlatform& Platform, uint16 Mapping,
                              const FGnmDescriptor& D)
{
    check(D.NumWords == 4 || D.NumWords == 8);
    if (Mapping != 0xffff)
    {
        const uint16 Index = Mapping & 0x3fff;
        if (Mapping & 0x8000)
            SetComputeUserData(Platform, Index, D.Words, D.NumWords, true);
        else
        {
            check(uint32(Index) + D.NumWords <= uint32(Staging.Num()));
            FMemory::Memcpy(Staging.GetData() + Index, D.Words, D.NumWords * sizeof(uint32));
        }
    }
    bDirty = true; // observed even for unused/direct mappings
}

// 0x4c39b50, basic CS table/pointer subset. SDK prefetch and +c/+d/+e special
// bindings are not implemented. The staging prefix must be supplied completely.
void FGnmComputeBindings::Commit(IGnmReferencePlatform& Platform,
                                const FGnmComputeReflection& R)
{
    checkf(!R.bHasUnrecoveredExtras, TEXT("Special LCUE bindings need reconstruction"));
    if (!bDirty) return;
    check(R.TableDwords <= Staging.Num());
    const uint32 Bytes = uint32(R.TableDwords) * 4;
    const FGnmAllocation A = Platform.Allocate(EGnmAllocationKind::LcueTable, Bytes, 4);
    check(A.Cpu && A.Capacity >= Bytes);
    if (Bytes) FMemory::Memcpy(A.Cpu, Staging.GetData(), Bytes);
    for (uint32 I = 0; I < 5; ++I)
    {
        if (R.PointerSgprs[I] == 255) continue;
        const uint64 Address = A.Gpu + (I == 0 ? 0 : 4u * R.SubtableDwords[I - 1]);
        const uint32 Pointer[2] = {uint32(Address), uint32(Address >> 32)};
        SetComputeUserData(Platform, R.PointerSgprs[I], Pointer, 2, false);
    }
    bDirty = false; // uploaded table is immutable; later setters modify CPU staging
}

FGnmCommandContext::FGnmCommandContext(IGnmReferencePlatform& InPlatform,
    FGnmUniformBuffer* InDefaultUniform, const TArray<uint8>& InitialLooseShadow,
    const TArray<uint32>& InitialStaging)
    : Platform(InPlatform), DefaultUniform(InDefaultUniform)
{
    ComputeLoose.Shadow = InitialLooseShadow;
    ComputeBindings.Staging = InitialStaging;
    // This is a clean reference session, NOT a recovered game-context constructor.
}

// 0x36cb00. Assigning shader metadata does not immediately bind the SDK shader.
void FGnmCommandContext::RHISetComputeShader(FComputeShaderRHIParamRef ShaderRHI)
{
    FGnmComputeShader* Shader = static_cast<FGnmComputeShader*>(ShaderRHI);
    check(Shader && Shader->NativeProgram);
    const uint64 OldProgram = ComputeShader ? ComputeShader->NativeProgram : 0;
    ComputeShader = Shader; // UE AddRef/Release, including old resource lifetime
    for (uint64& Entry : TrackedEntries) Entry = 0;
    bComputeShaderDirty |= OldProgram != Shader->NativeProgram;
    ComputeBindings.bDirty = true;
    ActiveStageFlags |= 1;
    DispatchMode = Shader->ProgramByte24 & 3;
    if (Shader->ProgramWord6)
    {
        const uint32 Value = ((uint32(Shader->ProgramWord6) * 1024 + 0xc00) & 0x1fff000) | 0x240;
        const uint32 Packet[] = {0xc0017602, 0x218, Value};
        Platform.EmitPacket(Packet, 3);
    }
    // reflection+d special case writes GNM+c3c8=0x40; excluded by the extras
    // precondition at dispatch. No all-UB-dirty or loose-dirty write in this function.
}

// 0x377bc0 -> 0x368f70. The compiler also inlines this into command Execute
// at 0x369390. The shader and BufferIndex arguments are unused at this entry.
void FGnmCommandContext::RHISetShaderParameter(FComputeShaderRHIParamRef ShaderRHI,
    uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue)
{
    (void)ShaderRHI;
    (void)BufferIndex;
    ComputeLoose.Write(uint16(BaseIndex), uint16(NumBytes), NewValue);
}

// 0x36a4c0: constants bind NOW; resource entries expand later at dispatch.
void FGnmCommandContext::RHISetShaderUniformBuffer(FComputeShaderRHIParamRef ShaderRHI,
    uint32 BufferIndex, FUniformBufferRHIParamRef BufferRHI)
{
    (void)ShaderRHI; // binary uses the context's current CS reflection instead
    check(ComputeShader && BufferIndex < 32);
    FGnmUniformBuffer* Buffer = static_cast<FGnmUniformBuffer*>(BufferRHI);
    FGnmUniformBuffer* ConstantSource = Buffer && Buffer->GetSize() ? Buffer : DefaultUniform.GetReference();
    check(ConstantSource); // default object at binary global 0xa726658 must be supplied
    if (ConstantSource->GetSize())
    {
        check(ComputeShader->Reflection.ConstantSlots.IsValidIndex(BufferIndex));
        ComputeBindings.Bind(Platform, ComputeShader->Reflection.ConstantSlots[BufferIndex],
                             ConstantSource->Constants);
    }
    ComputeUniformBuffers[BufferIndex] = Buffer; // retain ORIGINAL, even null/resources-only
    ComputeUniformDirty |= uint32(1) << BufferIndex; // context +0x91c
}

void FGnmCommandContext::CommitResourceMap(const TArray<uint32>& Map, uint32 Slot,
                                         EGnmResourceKind Kind)
{
    if (!Map.Num()) return;
    check(Map.IsValidIndex(Slot));
    uint32 Index = Map[Slot];
    if (!Index) return;
    const FGnmUniformBuffer* Uniform = ComputeUniformBuffers[Slot].GetReference();
    check(Uniform);
    for (;;)
    {
        check(Index < uint32(Map.Num()));
        const uint32 Entry = Map[Index++];
        if (FRHIResourceTableEntry::GetUniformBufferIndex(Entry) != Slot) break;
        const uint32 ResourceIndex = FRHIResourceTableEntry::GetResourceIndex(Entry);
        const uint32 BindIndex = FRHIResourceTableEntry::GetBindIndex(Entry);
        check(Uniform->Resources.IsValidIndex(ResourceIndex));
        const FGnmDescriptor D = Platform.ResolveDescriptor(Kind, Uniform->Resources[ResourceIndex].GetReference());
        const TArray<uint16>& Mappings = Kind == EGnmResourceKind::Sampler
            ? ComputeShader->Reflection.SamplerSlots : ComputeShader->Reflection.ResourceSlots;
        check(Mappings.IsValidIndex(BindIndex));
        ComputeBindings.Bind(Platform, Mappings[BindIndex], D);
    }
}

// 0x36db40, same packed entry fields as UE FRHIResourceTableEntry (8/16/8).
void FGnmCommandContext::CommitComputeResourceTables()
{
    const FGnmShaderResourceTable& Table = ComputeShader->ResourceTable;
    uint32 Active = Table.ResourceTableBits & ComputeUniformDirty;
    while (Active)
    {
        const uint32 Slot = FPlatformMath::CountTrailingZeros(Active);
        CommitResourceMap(Table.TextureMap, Slot, EGnmResourceKind::Texture);
        CommitResourceMap(Table.SRVMap, Slot, EGnmResourceKind::ShaderResourceView);
        CommitResourceMap(Table.SamplerMap, Slot, EGnmResourceKind::Sampler);
        Active &= Active - 1;
    }
    ComputeUniformDirty = 0; // clears even bits not used by this shader
}

void FGnmCommandContext::PrepareCompute(const FGnmAllocation* IndirectBacking)
{
    check(ComputeShader);
    checkf(!ComputeShader->Reflection.bHasUnrecoveredExtras, TEXT("Unsupported LCUE reflection extras"));
    if (bAutoFlush) Platform.FlushPriorGraphics(PendingGraphicsRanges);
    if (ComputeShader->bUsesLooseConstants)
    {
        FGnmDescriptor D;
        if (ComputeLoose.Commit(Platform, true, D))
        {
            check(ComputeShader->Reflection.ConstantSlots.Num());
            ComputeBindings.Bind(Platform, ComputeShader->Reflection.ConstantSlots[0], D);
            ComputeLoose.AcknowledgeBinding();
        }
    }
    CommitComputeResourceTables();
    if (IndirectBacking)
    {
        check(IndirectBacking->Gpu < (uint64(1) << 40) && !(IndirectBacking->Gpu & 7));
        const uint32 Sync[] = {0xc0004200, 0};
        const uint32 Base[] = {0xc0021102, 1, uint32(IndirectBacking->Gpu), uint32(IndirectBacking->Gpu >> 32)};
        Platform.EmitPacket(Sync, 2);
        Platform.EmitPacket(Base, 4);
    }
    if (bComputeShaderDirty) Platform.SetCsShaderWithModifier(ComputeShader->NativeProgram);
    ComputeBindings.Commit(Platform, ComputeShader->Reflection);
    bComputeShaderDirty = false;
}

void FGnmCommandContext::FinishCompute()
{
    if (bAutoFlush && bPostComputeFlush) Platform.FlushAfterCompute();
    Platform.FinishComputeBookkeeping();
}

// 0x36cfb0. This only RECORDS commands; it is not RHISubmitCommandsHint/flip.
void FGnmCommandContext::RHIDispatchComputeShader(uint32 ThreadGroupCountX,
    uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ)
{
    PrepareCompute(nullptr);
    Platform.DispatchDirect(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ,
                            (ActiveStageFlags & 1) + 8u * DispatchMode);
    FinishCompute();
}

// 0x3772d0. Resolve the buffer's current allocation at EXECUTION, not recording.
void FGnmCommandContext::RHIDispatchIndirectComputeShader(FVertexBufferRHIParamRef ArgumentBuffer,
                                                        uint32 ArgumentOffset)
{
    check(ArgumentBuffer);
    const FGnmAllocation& A = static_cast<FGnmVertexBuffer*>(ArgumentBuffer)->CurrentData();
    PrepareCompute(&A);
    Platform.DispatchIndirect(ArgumentOffset, (ActiveStageFlags & 1) + 8u * DispatchMode);
    FinishCompute();
}
