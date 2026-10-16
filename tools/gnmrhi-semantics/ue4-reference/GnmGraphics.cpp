#include "GnmRHIReference.h"

namespace
{
struct FGraphicsSink
{
    IGnmReferencePlatform& Platform;
    void Emit(const uint32* Words, uint32 Count) { Platform.EmitPacket(Words, Count); }
    uint64 UploadTable(const uint32* Words, uint32 Count)
    {
        const FGnmAllocation A = Platform.Allocate(EGnmAllocationKind::LcueTable, Count * 4, 4);
        check(A.Cpu && A.Capacity >= Count * 4);
        if (Count) FMemory::Memcpy(A.Cpu, Words, Count * 4);
        return A.Gpu;
    }
};

FGnmGraphicsStage& StageState(FGnmGraphicsState& G, uint32 Stage)
{
    check(Stage >= 1 && Stage <= 4);
    return Stage == 1 ? G.PS : Stage == 2 ? G.VS : Stage == 3 ? G.GS : G.ES;
}

void Select(FGnmGraphicsStage& State, const FGnmGraphicsShaderData* Next, uint32 KeyWords)
{
    const FGnmGraphicsShaderData* Old = State.Shader;
    State.Selection.Select(Old ? Old->NativeProgram : 0, Next ? Next->NativeProgram : 0,
                           Old ? Old->UpdateKey : nullptr, Next ? Next->UpdateKey : nullptr, KeyWords);
    State.Shader = Next;
}
}

// 0x382bf0; 0x384390 is its IRHIDeprecatedContext this-=8 thunk.
void FGnmCommandContext::RHISetBoundShaderState(FBoundShaderStateRHIParamRef BoundRHI)
{
    FGnmBoundShaderState* Next = static_cast<FGnmBoundShaderState*>(BoundRHI);
    check(Next && Next->Declaration && Next->VS && Next->VS->NativeProgram);
    checkf(!Next->bHasTessellation, TEXT("KH3 UE LS/HS selection bridge remains unproven"));
    check(!Next->PS || Next->PS->NativeProgram);
    Graphics.bVertexDirty = Graphics.bDiscardLooseExtent = true;
    if (Next->PS && Next->PS->Reflection.Lcue.Selector[13] != 255)
        Graphics.PS.Extras.ScalarD = 4 * Graphics.PendingPixelResourceCount;
    Platform.ApplyGraphicsSelectionSideEffects(Next->SharedWord, bool(Next->PS));
    FGraphicsSink Sink{Platform};
    const bool bGeometry = Next->GS && Next->VS->AlternateES.NativeProgram;
    checkf(!Next->GS || bGeometry, TEXT("GS without alternate ES is outside this reference"));
    Graphics.VertexStage = bGeometry ? 4 : 2;
    check(Next->VS->HardwareStage == Graphics.VertexStage);
    if (bGeometry)
    {
        FGnmGeometryShader& GS = *Next->GS;
        const auto& M = GS.Metadata;
        const auto Budget = M.OnChip() ? kh3_gfx::OnChipBudget(M) : kh3_gfx::GeometryBudget{};
        check(!M.OnChip() || Budget.Groups);
        Graphics.EsLds512Units = Budget.Lds512Units;
        Select(Graphics.ES, &Next->VS->AlternateES, 0);
        Graphics.ES.Selection.UpdateAllowed = false;
        Select(Graphics.GS, &GS, 3);
        check(GS.CopyShader.NativeProgram);
        Select(Graphics.VS, &GS.CopyShader, 3);
        Graphics.VS.Extras.FetchShader = 0;
        Graphics.ES.Extras.FetchShader = Next->FetchShader;
        Graphics.GeometryMode.Select(Sink, M, Budget.Groups);
        if (M.OnChip())
        {
            // Both GS and copy-VS consume the same WORD*WORD product at +f.
            if (GS.Reflection.Lcue.Selector[15] != 255)
            {
                const uint32 Product = uint16(Budget.Groups*((uint16(M.Word34)>>11)+1)) *
                                       uint32(uint16(Next->VS->EsWord1c));
                Graphics.GS.Extras.ScalarF = Graphics.VS.Extras.ScalarF = Product;
            }
            kh3_gfx::ContextRegister(Sink, 0x2ab, Next->VS->EsWord1c & 0x7fff);
            kh3_gfx::GeometryRingRegisters(Sink, M);
        }
        else if (!Graphics.RingKey.Matches(Next->VS->EsWord1c, M))
        {
            const FGnmGeometryRings Rings = Platform.GetGeometryRings();
            check(Rings.EsGs && Rings.GsVs);
            const FGnmAllocation Table = Platform.EmbedGeometryRingTable();
            check(Table.Cpu && Table.Capacity >= 192);
            uint32* Words = reinterpret_cast<uint32*>(Table.Cpu);
            for (uint32 I = 0; I < 2; ++I)
            {
                check(Rings.AuxiliaryTableSlot[I] < 12);
                FMemory::Memcpy(Words + 4 * Rings.AuxiliaryTableSlot[I], Rings.Auxiliary[I], 16);
            }
            kh3_gfx::GeometryRingDescriptors(Words, Rings.EsGs, Rings.GsVs, M);
            Graphics.RingTableGpu = Table.Gpu;
            kh3_gfx::ContextRegister(Sink, 0x2ab, Next->VS->EsWord1c & 0x7fff);
            kh3_gfx::GeometryRingRegisters(Sink, M);
            Graphics.RingKey.Remember(Next->VS->EsWord1c, M);
        }
        Graphics.ES.Extras.SharedPointer = Graphics.GS.Extras.SharedPointer =
            Graphics.VS.Extras.SharedPointer = Graphics.RingTableGpu;
    }
    else
    {
        Graphics.GeometryMode.Disable(Sink);
        Select(Graphics.VS, Next->VS.GetReference(), 3);
        Graphics.VS.Extras.FetchShader = Next->FetchShader;
    }
    Select(Graphics.PS, Next->PS.GetReference(), 8);
    // Keep Old alive until Selection has compared its program key.
    Graphics.Bound = Next;
    Graphics.History[Graphics.HistoryCursor] = Next;
    Graphics.HistoryCursor = (Graphics.HistoryCursor + 1) % 2500;
    Graphics.VS.UniformDirty = Graphics.PS.UniformDirty = Graphics.GS.UniformDirty = 0xfffff;
    ActiveStageFlags |= bGeometry ? 0x1e : 6;
    // The linker is recomputed on emission, equivalent to invalidating +c1c0.
    // Loose dirty itself is NOT set here.
}

// 0x37e180. Resolve CURRENT backing here, then store address/size/stride only.
void FGnmCommandContext::RHISetStreamSource(uint32 StreamIndex, FVertexBufferRHIParamRef BufferRHI,
                                           uint32 Stride, uint32 Offset)
{
    check(StreamIndex < 16);
    FGnmVertexBuffer* Buffer = static_cast<FGnmVertexBuffer*>(BufferRHI);
    Graphics.Streams[StreamIndex] = kh3_gfx::SnapshotStream(
        Buffer ? Buffer->CurrentData().Gpu : 0, Buffer ? Buffer->GetSize() : 0, Stride, Offset);
    Graphics.bVertexDirty = true;
}

void FGnmCommandContext::BindGraphics(uint32 Stage, uint16 Mapping, const FGnmDescriptor& D)
{
    FGraphicsSink Sink{Platform};
    FGnmGraphicsStage& S = StageState(Graphics, Stage);
    kh3_gfx::Bind(Sink, Stage, Mapping, D.Words, D.NumWords,
                 S.Staging.GetData(), S.Staging.Num(), S.Selection);
}

// VS/PS command Execute at 0x368f40 / 0x369340 reaches the same loose writer
// as compute. These public wrappers are semantic UE counterparts, not claimed
// standalone symbol addresses for those Execute functions.
void FGnmCommandContext::RHISetShaderParameter(FVertexShaderRHIParamRef Shader, uint32 BufferIndex,
    uint32 BaseIndex, uint32 NumBytes, const void* NewValue)
{
    (void)Shader; (void)BufferIndex;
    Graphics.VS.Loose.Write(uint16(BaseIndex), uint16(NumBytes), NewValue);
}
void FGnmCommandContext::RHISetShaderParameter(FPixelShaderRHIParamRef Shader, uint32 BufferIndex,
    uint32 BaseIndex, uint32 NumBytes, const void* NewValue)
{
    (void)Shader; (void)BufferIndex;
    Graphics.PS.Loose.Write(uint16(BaseIndex), uint16(NumBytes), NewValue);
}

void FGnmCommandContext::SetGraphicsUniform(uint32 Stage, uint32 Slot, FUniformBufferRHIParamRef BufferRHI)
{
    FGnmGraphicsStage& S = StageState(Graphics, Stage);
    check(S.Shader && Slot < 20);
    FGnmUniformBuffer* Buffer = static_cast<FGnmUniformBuffer*>(BufferRHI);
    FGnmUniformBuffer* Constants = Buffer && Buffer->GetSize() ? Buffer : DefaultUniform.GetReference();
    check(Constants);
    if (Constants->GetSize())
    {
        check(S.Shader->Reflection.ConstantSlots.IsValidIndex(Slot));
        BindGraphics(Stage, S.Shader->Reflection.ConstantSlots[Slot], Constants->Constants);
    }
    // UE vertex UB ownership is unchanged when its hardware stage moves to ES.
    FGnmGraphicsStage& Owner = Stage == 4 ? Graphics.VS : S;
    Owner.Uniforms[Slot] = Buffer;
    Owner.UniformDirty |= 1u << Slot;
}
// VS 0x369400 uses shader+380 to route constants; PS 0x36a130 uses stage 1.
void FGnmCommandContext::RHISetShaderUniformBuffer(FVertexShaderRHIParamRef Shader, uint32 Slot,
                                                 FUniformBufferRHIParamRef Buffer)
{
    check(Shader);
    SetGraphicsUniform(static_cast<FGnmVertexShader*>(Shader)->HardwareStage, Slot, Buffer);
}
void FGnmCommandContext::RHISetShaderUniformBuffer(FPixelShaderRHIParamRef Shader, uint32 Slot,
                                                 FUniformBufferRHIParamRef Buffer)
{
    (void)Shader; SetGraphicsUniform(1, Slot, Buffer);
}

void FGnmCommandContext::SetGraphicsResource(uint32 Stage, uint32 Slot,
                                            EGnmResourceKind Kind, FRHIResource* Resource)
{
    // Texture and SRV null setters are no-ops in this path, not zero descriptors.
    if (!Resource && Kind != EGnmResourceKind::Sampler) return;
    FGnmGraphicsStage& S = StageState(Graphics, Stage);
    check(S.Shader);
    const TArray<uint16>& Map = Kind == EGnmResourceKind::Sampler
        ? S.Shader->Reflection.SamplerSlots : S.Shader->Reflection.ResourceSlots;
    check(Map.IsValidIndex(Slot));
    if (Kind == EGnmResourceKind::Sampler && Map[Slot] == 0xffff)
    {
        S.Selection.ResourcesDirty = true;
        return; // unused sampler does not dereference the sampler object
    }
    check(Resource);
    BindGraphics(Stage, Map[Slot], Platform.ResolveDescriptor(Kind, Resource));
}

void FGnmCommandContext::RHISetShaderTexture(FVertexShaderRHIParamRef Shader, uint32 Slot,
                                            FTextureRHIParamRef Texture)
{
    check(Shader); SetGraphicsResource(static_cast<FGnmVertexShader*>(Shader)->HardwareStage,
                                      Slot, EGnmResourceKind::Texture, Texture);
}
void FGnmCommandContext::RHISetShaderTexture(FPixelShaderRHIParamRef Shader, uint32 Slot,
                                            FTextureRHIParamRef Texture)
{
    (void)Shader; SetGraphicsResource(1, Slot, EGnmResourceKind::Texture, Texture);
}
void FGnmCommandContext::RHISetShaderSampler(FVertexShaderRHIParamRef Shader, uint32 Slot,
                                            FSamplerStateRHIParamRef Sampler)
{
    check(Shader); SetGraphicsResource(static_cast<FGnmVertexShader*>(Shader)->HardwareStage,
                                      Slot, EGnmResourceKind::Sampler, Sampler);
}
void FGnmCommandContext::RHISetShaderSampler(FPixelShaderRHIParamRef Shader, uint32 Slot,
                                            FSamplerStateRHIParamRef Sampler)
{
    (void)Shader; SetGraphicsResource(1, Slot, EGnmResourceKind::Sampler, Sampler);
}
void FGnmCommandContext::RHISetShaderResourceViewParameter(FVertexShaderRHIParamRef Shader, uint32 Slot,
                                                         FShaderResourceViewRHIParamRef SRV)
{
    check(Shader); SetGraphicsResource(static_cast<FGnmVertexShader*>(Shader)->HardwareStage,
                                      Slot, EGnmResourceKind::ShaderResourceView, SRV);
}
void FGnmCommandContext::RHISetShaderResourceViewParameter(FPixelShaderRHIParamRef Shader, uint32 Slot,
                                                         FShaderResourceViewRHIParamRef SRV)
{
    (void)Shader; SetGraphicsResource(1, Slot, EGnmResourceKind::ShaderResourceView, SRV);
}

// 0x384e20 / 0x3854b0: independent packed maps, each with its own UB prefix.
void FGnmCommandContext::CommitGraphicsResourceTables(uint32 Stage)
{
    FGnmGraphicsStage& S = StageState(Graphics, Stage);
    if (!S.Shader) return;
    FGnmGraphicsStage& Owner = Stage == 4 ? Graphics.VS : S;
    const FGnmShaderResourceTable& Table = Stage == 4 ? Graphics.Bound->VS->ResourceTable : S.Shader->ResourceTable;
    uint32 Active = Owner.UniformDirty & Table.ResourceTableBits;
    check(!(Active & ~0xfffffu));
    auto Expand = [&](const TArray<uint32>& Map, uint32 Slot, EGnmResourceKind Kind) {
        if (!Map.Num()) return;
        check(Map.IsValidIndex(Slot));
        uint32 Index = Map[Slot];
        if (!Index) return;
        FGnmUniformBuffer* Uniform = Owner.Uniforms[Slot].GetReference();
        check(Uniform);
        for (;;)
        {
            check(Map.IsValidIndex(Index));
            const uint32 Entry = Map[Index++];
            if (FRHIResourceTableEntry::GetUniformBufferIndex(Entry) != Slot) break;
            const uint32 Resource = FRHIResourceTableEntry::GetResourceIndex(Entry);
            check(Uniform->Resources.IsValidIndex(Resource));
            SetGraphicsResource(Stage, FRHIResourceTableEntry::GetBindIndex(Entry), Kind,
                                Uniform->Resources[Resource].GetReference());
        }
    };
    while (Active)
    {
        const uint32 Slot = FPlatformMath::CountTrailingZeros(Active);
        Expand(Table.TextureMap, Slot, EGnmResourceKind::Texture);
        Expand(Table.SRVMap, Slot, EGnmResourceKind::ShaderResourceView);
        Expand(Table.SamplerMap, Slot, EGnmResourceKind::Sampler);
        Active &= Active - 1;
    }
    Owner.UniformDirty = 0;
}

// 0x386a40 -> 0x384e20 / 0x3854b0 -> 0x386680.
void FGnmCommandContext::PrepareGraphics(uint32 FirstInstance)
{
    check(Graphics.Bound);
    FGnmBoundShaderState& Bound = *Graphics.Bound;
    Graphics.bVertexDirty |= Graphics.FirstInstance != FirstInstance;
    if (Graphics.bVertexDirty)
    {
        Graphics.FirstInstance = FirstInstance;
        const auto& Elements = Bound.Declaration->Elements;
        check(Bound.AttributeRemap.Num() >= Elements.Num());
        for (int32 I = 0; I < Elements.Num(); ++I)
        {
            const int32 Attribute = Bound.AttributeRemap[I];
            if (Attribute < 0) continue;
            const FVertexElement& E = Elements[I];
            check(E.StreamIndex < 16);
            const kh3_gfx::Stream& Stream = Graphics.Streams[E.StreamIndex];
            if (!Stream.Base) continue;
            FGnmDescriptor D; D.NumWords = 4;
            kh3_gfx::VertexDescriptor(Stream, E.Offset, uint8(E.Type), E.Stride,
                                     E.bUseInstanceIndex != 0, FirstInstance, D.Words);
            const auto& Slots = StageState(Graphics, Graphics.VertexStage).Shader->Reflection.VertexSlots;
            check(Slots.IsValidIndex(Attribute));
            BindGraphics(Graphics.VertexStage, Slots[Attribute], D);
        }
        Graphics.bVertexDirty = false;
    }
    CommitGraphicsResourceTables(Graphics.VertexStage);
    CommitGraphicsResourceTables(1);
    if (Bound.GS) CommitGraphicsResourceTables(3);
    auto Loose = [&](uint32 Stage, bool Used) {
        if (!Used) return;
        FGnmGraphicsStage& S = StageState(Graphics, Stage);
        check(S.Shader && S.Shader->Reflection.ConstantSlots.Num());
        FGnmDescriptor D;
        FGnmLooseConstants& Constants = Stage == 4 ? Graphics.VS.Loose : S.Loose;
        if (Constants.Commit(Platform, Graphics.bDiscardLooseExtent, D))
        {
            BindGraphics(Stage, S.Shader->Reflection.ConstantSlots[0], D);
            Constants.AcknowledgeBinding();
        }
    };
    Loose(Graphics.VertexStage, Bound.bUsesVertexLoose);
    Loose(1, Bound.bUsesPixelLoose);
    Loose(3, Bound.bUsesGeometryLoose);
    Graphics.bDiscardLooseExtent = false;
}

// VS/PS and ES/GS arms of 0x4c3a220. It RECORDS DCB + descriptor snapshots;
// it does not call sceGnmSubmitCommandBuffers, submitDone or flip.
void FGnmCommandContext::CommitGraphicsLcue()
{
    FGraphicsSink Sink{Platform};
    FGnmGraphicsStage& VS = Graphics.VS;
    FGnmGraphicsStage& PS = Graphics.PS;
    auto Program = [&](FGnmGraphicsStage& S, uint32 Stage, uint32 Modifier) {
        if (!S.Selection.ShaderDirty) return;
        const uint64 Address = S.Shader ? S.Shader->NativeProgram : 0;
        Platform.SetGraphicsShader(Stage, Address, Modifier, S.Selection.UpdateAllowed);
        if (Graphics.bPrefetch && Address) Platform.PrefetchGraphicsShader(Address);
    };
    auto Table = [&](FGnmGraphicsStage& S, uint32 Stage) {
        if (!S.Selection.ResourcesDirty) return;
        check(S.Shader);
        kh3_gfx::CommitTable(Sink, Stage, S.Shader->Reflection.Lcue, S.Extras,
                            S.Staging.GetData(), S.Staging.Num());
    };
    if (Graphics.VertexStage == 4)
    {
        FGnmGraphicsStage& ES = Graphics.ES;
        if (ES.Selection.ShaderDirty)
        {
            Platform.SetExportShader(ES.Shader->NativeProgram, Graphics.Bound->Modifier,
                kh3_gfx::PatchEsResource2(Graphics.Bound->VS->EsWord14, Graphics.EsLds512Units));
            if (Graphics.bPrefetch) Platform.PrefetchGraphicsShader(ES.Shader->NativeProgram);
        }
        Program(Graphics.GS, 3, 0);
        Table(ES, 4); Table(Graphics.GS, 3);
        ES.Selection.Acknowledge(false); Graphics.GS.Selection.Acknowledge();
    }
    // ES+GS programs/tables precede copyVS+PS programs/tables in 0x4c3a220.
    Program(VS, 2, Graphics.VertexStage == 4 ? 0 : Graphics.Bound->Modifier);
    Program(PS, 1, 0);
    Table(VS, 2);
    Table(PS, 1);
    if ((VS.Selection.ShaderDirty || PS.Selection.ShaderDirty) && VS.Shader && PS.Shader)
    {
        const TArray<uint16>& Inputs = PS.Shader->Semantics;
        check(Inputs.Num() <= 32);
        if (Inputs.Num())
        {
            uint32 Packet[34];
            Packet[0] = 0xc0006900 | (uint32(Inputs.Num()) << 16);
            Packet[1] = 0x191; // SPI_PS_INPUT_CNTL_0, helper 0x68bb930
            for (int32 I = 0; I < Inputs.Num(); ++I)
                Packet[I + 2] = kh3_gfx::LinkPixelInput(Inputs[I], VS.Shader->Semantics.GetData(),
                                                      VS.Shader->Semantics.Num(), Graphics.bNeo);
            Sink.Emit(Packet, Inputs.Num() + 2);
        }
    }
    VS.Selection.Acknowledge();
    PS.Selection.Acknowledge();
}

// UE GS counterparts. The native GS UB setter is 0x369da0; packed UB resource
// expansion is 0x386240. Command Execute wrappers are not public RHI symbols.
void FGnmCommandContext::RHISetShaderParameter(FGeometryShaderRHIParamRef Shader,
    uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue)
{
    (void)Shader; (void)BufferIndex;
    Graphics.GS.Loose.Write(uint16(BaseIndex), uint16(NumBytes), NewValue);
}
void FGnmCommandContext::RHISetShaderUniformBuffer(FGeometryShaderRHIParamRef Shader,
    uint32 Slot, FUniformBufferRHIParamRef Buffer)
{
    (void)Shader; SetGraphicsUniform(3, Slot, Buffer);
}
void FGnmCommandContext::RHISetShaderTexture(FGeometryShaderRHIParamRef Shader,
    uint32 Slot, FTextureRHIParamRef Texture)
{
    (void)Shader; SetGraphicsResource(3, Slot, EGnmResourceKind::Texture, Texture);
}
void FGnmCommandContext::RHISetShaderSampler(FGeometryShaderRHIParamRef Shader,
    uint32 Slot, FSamplerStateRHIParamRef Sampler)
{
    (void)Shader; SetGraphicsResource(3, Slot, EGnmResourceKind::Sampler, Sampler);
}
void FGnmCommandContext::RHISetShaderResourceViewParameter(FGeometryShaderRHIParamRef Shader,
    uint32 Slot, FShaderResourceViewRHIParamRef SRV)
{
    (void)Shader; SetGraphicsResource(3, Slot, EGnmResourceKind::ShaderResourceView, SRV);
}

void FGnmCommandContext::RHIDrawPrimitive(uint32 PrimitiveType, uint32 BaseVertexIndex,
                                         uint32 NumPrimitives, uint32 NumInstances)
{
    uint32 Topology = 0, Count = 0;
    const bool bSupported = kh3_gfx::DrawShape(PrimitiveType, NumPrimitives, Topology, Count);
    checkf(bSupported,
           TEXT("Only triangle list/strip and line list are closed for this draw path"));
    PrepareGraphics(0);
    FGraphicsSink Sink{Platform};
    Graphics.Draw.SetTopology(Sink, Topology, Graphics.bNeo);
    Graphics.Draw.SetBase(Sink, BaseVertexIndex);
    Graphics.Draw.SetInstances(Sink, NumInstances);
    Graphics.Draw.SetIndexType(Sink, 0);
    CommitGraphicsLcue();
    Platform.DrawIndexAuto(Count, Graphics.DrawFlags & 1);
    Graphics.Draw.AfterDraw(Sink);
}

void FGnmCommandContext::RHIDrawIndexedPrimitive(FIndexBufferRHIParamRef IndexBuffer,
    uint32 PrimitiveType, int32 BaseVertexIndex, uint32 FirstInstance, uint32 NumVertices,
    uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances)
{
    (void)NumVertices; // unused in recovered direct entry 0x381550
    check(IndexBuffer);
    uint32 Topology = 0, Count = 0;
    const bool bSupported = kh3_gfx::DrawShape(PrimitiveType, NumPrimitives, Topology, Count);
    checkf(bSupported,
           TEXT("Unsupported primitive topology in recovered direct path"));
    const uint32 IndexType = Platform.GetRawIndexType(IndexBuffer);
    const uint32 ByteOffset = StartIndex * (IndexType ? 4u : 2u);
    PrepareGraphics(FirstInstance);
    FGraphicsSink Sink{Platform};
    Graphics.Draw.SetTopology(Sink, Topology, Graphics.bNeo);
    Graphics.Draw.SetInstances(Sink, NumInstances);
    Graphics.Draw.SetIndexType(Sink, IndexType);
    Graphics.Draw.SetBase(Sink, uint32(BaseVertexIndex));
    const uint64 Address = Platform.ResolveCurrentIndexAddress(IndexBuffer) + ByteOffset;
    CommitGraphicsLcue();
    Platform.DrawIndex(Count, Address, Graphics.DrawFlags & 1, 1);
    Graphics.Draw.AfterDraw(Sink);
}
