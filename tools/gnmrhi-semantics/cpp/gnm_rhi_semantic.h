#pragma once

// KH3 CUSA15072 01.10 / UE4.17-derived semantic reconstruction.
// These are analysis types, NOT the recovered binary layouts or Epic/Sony headers.
// See docs/gnm/kh3-gnmrhi-binding-semantics.md and the SHA-pinned symbol package.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace kh3::gnm {

using GpuAddress = std::uint64_t;
using ResourceHandle = std::uint64_t;
using BufferDescriptor = std::array<std::uint32_t, 4>;

enum class HardwareStage : std::uint8_t { CS, PS, VS, GS, ES, HS, LS };
enum class ResourceKind : std::uint8_t { Texture, Srv, Sampler };
enum class UploadKind { ConstantsIn64KiBBlock, TransientConstants, LcueTable };

// Adapters own memory lifetime and real SDK integration. An allocation must remain
// valid until the command stream referencing it is no longer in flight.
class Backend {
public:
    virtual ~Backend() = default;
    virtual GpuAddress AllocateUpload(UploadKind kind, std::uint32_t bytes,
                                     std::uint32_t alignment) = 0;
    virtual void WriteUpload(GpuAddress address, std::span<const std::byte> bytes) = 0;
    virtual void EmitPacket(std::span<const std::uint32_t> words) = 0;
    // A real adapter embeds this label in a NOP payload and CPU-zeros all 8 bytes.
    virtual GpuAddress ReserveZeroedLabel8() = 0;
    // SDK internals are intentionally delegated, not replaced with guessed packets.
    virtual void BindComputeShader(std::uint64_t shader_token) = 0;
    virtual void DispatchDirect(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                                std::uint32_t flags) = 0;
    virtual void DispatchIndirect(std::uint32_t offset, std::uint32_t flags) = 0;
};

struct ConstantUpload {
    GpuAddress address{};
    std::uint32_t extent{};       // Descriptor extent, not physical pool reservation.
    std::uint32_t copied_bytes{}; // Alignment padding is not initialized here.
    BufferDescriptor descriptor{};
};

class LooseConstants {
public:
    // The caller supplies initial shadow bytes; zero-init is not inferred from KH3.
    explicit LooseConstants(std::vector<std::byte> initial_shadow);
    void Write(std::uint32_t offset, std::span<const std::byte> bytes); // 0x368f70
    std::optional<ConstantUpload> Commit(Backend&, bool discard_old_extent); // 0x36d740
    void AcknowledgeBinding(); // Caller behavior, not part of Commit.
    bool Dirty() const { return dirty_; }
    std::uint32_t UpdatedEnd() const { return updated_end_; }
    std::uint32_t PriorExtent() const { return prior_extent_; }

private:
    std::vector<std::byte> shadow_;
    bool dirty_{};
    std::uint32_t updated_end_{};
    std::uint32_t prior_extent_{};
};

struct ResourceEntry {
    std::uint8_t uniform_slot{};
    std::uint16_t resource_index{};
    std::uint8_t bind_index{};
    static ResourceEntry Decode(std::uint32_t word);
};

enum class BindingRoute { Unused, Table, UserSgpr };
struct BindingLocation {
    BindingRoute route{};
    std::uint16_t index{};
    static BindingLocation Decode(std::uint16_t raw);
};

struct Descriptor {
    std::array<std::uint32_t, 8> words{};
    std::uint8_t count{};
    static Descriptor Buffer(const BufferDescriptor&);
    static Descriptor Texture(const std::array<std::uint32_t, 8>&);
    std::span<const std::uint32_t> Words() const;
};

// The adapter resolves texture references/virtual calls and current buffer backing.
// Captured object handles are opaque and never cast to host pointers by this library.
class ResourceResolver {
public:
    virtual ~ResourceResolver() = default;
    virtual Descriptor Resolve(ResourceKind kind, ResourceHandle handle) = 0;
};

struct UniformBuffer {
    BufferDescriptor constants{};
    std::vector<ResourceHandle> resources;
};
struct UniformBindings {
    std::array<const UniformBuffer*, 32> buffers{};
    std::uint32_t dirty_mask{}; // External input here; recovered CS setters are in ue4-reference/.
};
struct ShaderResourceTable {
    std::uint32_t uniform_mask{};
    // Texture, SRV, sampler order. Each contains its own UB-slot prefix.
    std::array<std::vector<std::uint32_t>, 3> maps;
};

struct ComputeReflection {
    std::uint16_t table_dwords{};
    std::vector<std::uint16_t> resource_slots; // raw mappings at +0x22
    std::vector<std::uint16_t> sampler_slots;  // raw mappings at +0xa2
    std::uint16_t constant_slot0{0xFFFF};      // raw mapping at +0xc2
    std::array<std::uint8_t, 5> pointer_sgprs{255, 255, 255, 255, 255}; // +7..+b
    std::array<std::uint16_t, 4> subtable_dwords{}; // +16/+1a/+1c/+1e (hex)
    // If reflection +c/+d/+e are used, caller must set this and extend the adapter.
    bool has_unmodeled_extra_bindings{};
    std::uint16_t Slot(ResourceKind kind, std::uint8_t bind_index) const;
};

struct TableUpload {
    GpuAddress address{};
    std::vector<std::uint32_t> words; // Immutable diagnostic snapshot.
};

class StageBindings {
public:
    explicit StageBindings(HardwareStage stage);
    void SeedStaging(std::span<const std::uint32_t> captured_prefix);
    void Bind(Backend&, std::uint16_t mapping, const Descriptor&);
    std::optional<TableUpload> CommitCompute(Backend&, const ComputeReflection&); // 0x4c39b50
    bool Dirty() const { return dirty_; }

private:
    HardwareStage stage_;
    std::array<std::optional<std::uint32_t>, 0x1800 / 4> shadow_;
    bool dirty_{};
};

void CommitResourceTables(Backend&, ResourceResolver&, const ShaderResourceTable&,
                          const ComputeReflection&, UniformBindings&, StageBindings&);
BufferDescriptor RefreshBufferSrv(BufferDescriptor descriptor, GpuAddress current_backing);

struct FlushRange { GpuAddress base{}; std::uint32_t bytes{}; };
void FlushPriorGraphics(Backend&, std::vector<FlushRange>&); // 0x36d440
void FlushAfterCompute(Backend&);                          // 0x36cfb0 tail

struct ComputeShader {
    std::uint64_t backend_token{};
    bool uses_loose_constants{};
    ShaderResourceTable resources;
    ComputeReflection reflection;
};
struct ComputeContext {
    const ComputeShader* shader{};
    LooseConstants* loose{};
    UniformBindings uniforms;
    StageBindings descriptors{HardwareStage::CS};
    std::vector<FlushRange> pending_graphics_ranges;
    bool shader_dirty{};
    bool auto_flush{};
    bool post_compute_flush{};
    std::uint32_t dispatch_flags{}; // Captured (gnm.flags&1)+8*gnm.byte_c9c1.
};

class GnmRhi {
public:
    GnmRhi(Backend& backend, ResourceResolver& resolver);
    void DispatchCompute(ComputeContext&, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    // current_backing must be resolved from the indirect RHI buffer at dispatch time.
    void DispatchComputeIndirect(ComputeContext&, GpuAddress current_backing, std::uint32_t offset);

private:
    void PrepareCompute(ComputeContext&, std::optional<GpuAddress> indirect_base = std::nullopt);
    void FinishCompute(ComputeContext&);
    Backend& backend_;
    ResourceResolver& resolver_;
};

// Previous reconstruction: 0x3a3010/0x39a100. No frame/fence logic is added to Lock.
enum BufferUsage : std::uint32_t { Static = 1, Dynamic = 2, Volatile = 4, Uav = 8 };
std::size_t BufferSlotCount(std::uint32_t usage);
class MultiBuffer {
public:
    MultiBuffer(std::uint32_t usage, std::uint32_t allocation_size,
                std::vector<GpuAddress> existing_slots, std::size_t current_slot);
    GpuAddress Lock(std::uint32_t raw_mode, std::uint32_t offset,
                    const std::function<GpuAddress(std::uint32_t)>& allocate_transient);
    GpuAddress CurrentData() const;
    std::size_t CurrentSlot() const { return current_slot_; }
    void Unlock() {} // 0x3cbeb0

private:
    std::uint32_t usage_;
    std::uint32_t allocation_size_;
    std::vector<GpuAddress> slots_;
    std::size_t current_slot_;
};

// 0x37d6210/0x37d6b20. Single-threaded semantic core; no inferred locking policy.
class DeferredReleaseQueue {
public:
    explicit DeferredReleaseQueue(std::uint64_t initial_ticket) : ticket_(initial_ticket) {}
    void Enqueue(ResourceHandle allocation);
    std::uint64_t RetireAndAdvance(std::uint64_t completed_ticket,
                                  const std::function<void(ResourceHandle)>& release);
    std::size_t Pending() const { return entries_.size(); }

private:
    struct Entry { std::uint64_t ticket; ResourceHandle allocation; };
    std::uint64_t ticket_;
    std::vector<Entry> entries_;
};

struct OcclusionPair { std::uint64_t begin; std::uint64_t end; };
// Pure readiness/aggregation core only. No invented polling, timeout, or cache policy.
std::optional<std::uint64_t> ReadOcclusion(std::span<const OcclusionPair> pairs, bool neo);
std::optional<std::uint64_t> ReadTimestamp(std::uint64_t timestamp, bool neo);

} // namespace kh3::gnm
