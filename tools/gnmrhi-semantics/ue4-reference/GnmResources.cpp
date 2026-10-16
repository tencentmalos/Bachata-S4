#include "GnmRHIReference.h"

namespace
{
FGnmDescriptor MakeConstantDescriptor(uint64 Address, uint32 Bytes)
{
    check(Address < (uint64(1) << 44));
    check(Bytes <= MAX_uint32 - 15);
    FGnmDescriptor D;
    D.NumWords = 4;
    D.Words[0] = uint32(Address);
    D.Words[1] = uint32((Address >> 32) & 0xfff) | 0x100000;
    D.Words[2] = (Bytes + 15) / 16;
    D.Words[3] = 0x20077fac;
    return D;
}
}

// 0x391d20 + 0x3a3ce0. The UE layout's resource offset is NOT necessarily
// ConstantBufferSize; resources are pointers rather than bytes of GNM descriptors.
FGnmUniformBuffer::FGnmUniformBuffer(IGnmReferencePlatform& InPlatform, const void* Contents,
    const FRHIUniformBufferLayout& Layout, EUniformBufferUsage Usage)
    : FGnmOwnedUniformLayout(Layout), FRHIUniformBuffer(OwnedLayout), Platform(InPlatform),
      bTransient(uint32(Usage) <= 1)
{
    check(Contents || (!Layout.ConstantBufferSize && !Layout.Resources.Num()));
    const uint32 Bytes = Layout.ConstantBufferSize;
    if (Bytes)
    {
        check(Bytes <= MAX_uint32 - 15);
        Allocation = Platform.Allocate(bTransient ? EGnmAllocationKind::UniformTransient
                                                  : EGnmAllocationKind::UniformPersistent,
                                       (Bytes + 15) & ~15u, 16);
        check(Allocation.Cpu && Allocation.Capacity >= ((Bytes + 15) & ~15u));
        FMemory::Memcpy(Allocation.Cpu, Contents, Bytes);
        Constants = MakeConstantDescriptor(Allocation.Gpu, Bytes);
    }
    Resources.Reserve(Layout.Resources.Num());
    for (int32 Index = 0; Index < Layout.Resources.Num(); ++Index)
    {
        FRHIResource* Resource = nullptr;
        FMemory::Memcpy(&Resource, static_cast<const uint8*>(Contents) + Layout.ResourceOffset
                                  + Index * sizeof(Resource), sizeof(Resource));
        Resources.Add(TRefCountPtr<FRHIResource>(Resource)); // owns the resource reference
    }
}

FGnmUniformBuffer::~FGnmUniformBuffer()
{
    if (Allocation.Cpu)
        Platform.RetireUniform(Allocation, bTransient);
    // Resources then releases UE refs. The adapter must outlive all these objects.
}

FUniformBufferRHIRef FGnmDynamicRHI::RHICreateUniformBuffer(const void* Contents,
    const FRHIUniformBufferLayout& Layout, EUniformBufferUsage Usage)
{
    return FUniformBufferRHIRef(new FGnmUniformBuffer(Platform, Contents, Layout, Usage));
}

// 0x368f70. The CS public wrapper has already narrowed Offset and NumBytes.
void FGnmLooseConstants::Write(uint16 Offset, uint16 NumBytes, const void* Source)
{
    check(uint32(Offset) + NumBytes <= uint32(Shadow.Num())); // reference guard
    if (NumBytes) FMemory::Memcpy(Shadow.GetData() + Offset, Source, NumBytes);
    UpdatedEnd = FMath::Max(UpdatedEnd, uint32(Offset) + NumBytes);
    bDirty = true;
}

// 0x36d740. Commit produces fresh backing; only the caller clears dirty/end.
bool FGnmLooseConstants::Commit(IGnmReferencePlatform& Platform, bool bDiscardOldExtent,
                               FGnmDescriptor& OutDescriptor)
{
    if (!bDirty) return false;
    const uint32 Bytes = bDiscardOldExtent ? UpdatedEnd : FMath::Max(UpdatedEnd, PriorExtent);
    check(Bytes <= MAX_uint32 - 15);
    const uint32 Extent = (Bytes + 15) & ~15u;
    check(Extent <= uint32(Shadow.Num()));
    PriorExtent = Extent;
    const FGnmAllocation A = Platform.Allocate(Extent > 65536 ? EGnmAllocationKind::LooseLarge
                                                            : EGnmAllocationKind::LooseIn64KiBBlock,
                                              Extent, 16);
    check(A.Cpu && A.Capacity >= Extent);
    if (Bytes) FMemory::Memcpy(A.Cpu, Shadow.GetData(), Bytes); // padding is unspecified
    OutDescriptor = MakeConstantDescriptor(A.Gpu, Extent);
    return true;
}

// 0x3a3010. Priority matters: Dynamic | UAV has ONE backing, not three.
FGnmVertexBuffer::FGnmVertexBuffer(uint32 Size, uint32 Usage,
    const TArray<FGnmAllocation>& Slots, uint32 InCurrentSlot)
    : FRHIVertexBuffer(Size, Usage), BackingSlots(Slots), CurrentSlot(InCurrentSlot)
{
    const uint32 Count = (Usage & BUF_Volatile) ? 1 :
        (Usage & (BUF_Static | BUF_UnorderedAccess)) ? 1 : (Usage & BUF_Dynamic) ? 3 : 0;
    check(Count && uint32(Slots.Num()) == Count && CurrentSlot < Count);
}

const FGnmAllocation& FGnmVertexBuffer::CurrentData() const
{
    return BackingSlots[(GetUsage() & BUF_Volatile) ? 0 : CurrentSlot];
}

// 0x39a100. A write Lock, not a frame tick, rotates the ordinary backing ring.
void* FGnmVertexBuffer::Lock(IGnmReferencePlatform& Platform, uint32 Offset,
                           EResourceLockMode Mode)
{
    if (uint32(Mode) != 0)
    {
        if (GetUsage() & BUF_Volatile)
            BackingSlots[0] = Platform.Allocate(EGnmAllocationKind::VolatileBuffer, GetSize(), 1024);
        else
            CurrentSlot = (CurrentSlot + 1) % BackingSlots.Num();
    }
    const FGnmAllocation& A = CurrentData();
    check(A.Cpu && Offset <= A.Capacity);
    return A.Cpu + Offset;
}

void* FGnmDynamicRHI::RHILockVertexBuffer(FVertexBufferRHIParamRef VertexBuffer,
    uint32 Offset, uint32 SizeRHI, EResourceLockMode LockMode)
{
    check(VertexBuffer && Offset <= VertexBuffer->GetSize());
    check(SizeRHI <= VertexBuffer->GetSize() - Offset); // reference guard
    return static_cast<FGnmVertexBuffer*>(VertexBuffer)->Lock(Platform, Offset, LockMode);
}

void FGnmDynamicRHI::RHIUnlockVertexBuffer(FVertexBufferRHIParamRef VertexBuffer)
{
    (void)VertexBuffer; // 0x3cbeb0 is a return. No inferred flush or GPU wait here.
}
