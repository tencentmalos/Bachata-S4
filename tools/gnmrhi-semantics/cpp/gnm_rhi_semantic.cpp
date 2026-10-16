#include "gnm_rhi_semantic.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace kh3::gnm {
static_assert(std::endian::native == std::endian::little,
              "The upload serializer currently requires a little-endian host");
namespace {
constexpr std::array<std::uint16_t, 7> UserBases{0x2e40, 0x2c0c, 0x2c4c, 0x2c8c,
                                              0x2ccc, 0x2d0c, 0x2d4c};

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

std::uint32_t Align16(std::uint32_t bytes) {
    Require(bytes <= std::numeric_limits<std::uint32_t>::max() - 15, "extent overflow");
    return (bytes + 15u) & ~15u;
}

void Emit(Backend& backend, std::initializer_list<std::uint32_t> words) {
    backend.EmitPacket({words.begin(), words.size()});
}

void EmitUserData(Backend& backend, HardwareStage stage, std::uint16_t index,
                  std::span<const std::uint32_t> words, bool descriptor_marker) {
    const auto stage_index = static_cast<std::size_t>(stage);
    Require(stage_index < UserBases.size(), "unknown GNM hardware stage");
    Require(!words.empty() && words.size() <= 8, "unsupported user-data write width");
    std::vector<std::uint32_t> packet;
    if (descriptor_marker) packet = {0xc0001000, 0x6875000d};
    packet.push_back(0xc0007600 | (static_cast<std::uint32_t>(words.size()) << 16) |
                     (stage == HardwareStage::CS ? 2u : 0u));
    packet.push_back(UserBases[stage_index] + index - 0x2c00u);
    packet.insert(packet.end(), words.begin(), words.end());
    backend.EmitPacket(packet);
}

GpuAddress NewLabel(Backend& backend) {
    const auto label = backend.ReserveZeroedLabel8();
    Require(label > 0xffff && label < (1ull << 40) && (label & 7) == 0,
            "expected aligned PS4 memory label, not register-space or truncated address");
    return label;
}

void WaitForLabel(Backend& backend, GpuAddress label) {
    Emit(backend, {0xc0053c00, 0x13, static_cast<std::uint32_t>(label) & ~3u,
                   static_cast<std::uint32_t>((label >> 32) & 0xffff), 1, 0xffffffff, 10});
}

void AcquireComputeCaches(Backend& backend) {
    Emit(backend, {0xc0055800, 0x00c40000, 1, 0, 0, 0, 10});
}
} // namespace

LooseConstants::LooseConstants(std::vector<std::byte> initial_shadow)
    : shadow_(std::move(initial_shadow)) {}

void LooseConstants::Write(std::uint32_t offset, std::span<const std::byte> bytes) {
    // Analysis guards, not inferred checks in the binary. High-water uses size's low16.
    Require(bytes.size() <= 0xffff, "update larger than modeled uint16 high-water domain");
    Require(offset <= shadow_.size() && bytes.size() <= shadow_.size() - offset,
            "constant update outside supplied shadow");
    Require(bytes.size() <= std::numeric_limits<std::uint32_t>::max() - offset,
            "constant update overflow");
    std::copy(bytes.begin(), bytes.end(), shadow_.begin() + offset);
    updated_end_ = std::max(updated_end_, offset + static_cast<std::uint32_t>(bytes.size()));
    dirty_ = true;
}

std::optional<ConstantUpload> LooseConstants::Commit(Backend& backend, bool discard_old_extent) {
    if (!dirty_) return std::nullopt;
    const auto bytes = discard_old_extent ? updated_end_ : std::max(updated_end_, prior_extent_);
    const auto extent = Align16(bytes);
    Require(extent <= shadow_.size(), "shadow does not cover aligned extent");
    prior_extent_ = extent;
    const auto kind = extent > 65536 ? UploadKind::TransientConstants
                                     : UploadKind::ConstantsIn64KiBBlock;
    const auto address = backend.AllocateUpload(kind, extent, 16);
    Require(address < (1ull << 44), "buffer address exceeds descriptor's observed 44 bits");
    backend.WriteUpload(address, std::span<const std::byte>(shadow_).first(bytes));
    return ConstantUpload{address, extent, bytes,
                          {static_cast<std::uint32_t>(address),
                           static_cast<std::uint32_t>((address >> 32) & 0xfff) | 0x100000,
                           extent / 16, 0x20077fac}};
}

void LooseConstants::AcknowledgeBinding() {
    dirty_ = false;
    updated_end_ = 0;
}

ResourceEntry ResourceEntry::Decode(std::uint32_t word) {
    return {static_cast<std::uint8_t>(word >> 24), static_cast<std::uint16_t>(word >> 8),
            static_cast<std::uint8_t>(word)};
}

BindingLocation BindingLocation::Decode(std::uint16_t raw) {
    if (raw == 0xffff) return {BindingRoute::Unused, 0};
    return {(raw & 0x8000) ? BindingRoute::UserSgpr : BindingRoute::Table,
            static_cast<std::uint16_t>(raw & 0x3fff)};
}

Descriptor Descriptor::Buffer(const BufferDescriptor& buffer) {
    Descriptor result;
    result.count = 4;
    std::copy(buffer.begin(), buffer.end(), result.words.begin());
    return result;
}

Descriptor Descriptor::Texture(const std::array<std::uint32_t, 8>& texture) {
    return {texture, 8};
}

std::span<const std::uint32_t> Descriptor::Words() const {
    Require(count == 4 || count == 8, "descriptor must contain 4 or 8 dwords");
    return std::span<const std::uint32_t>(words).first(count);
}

std::uint16_t ComputeReflection::Slot(ResourceKind kind, std::uint8_t bind_index) const {
    const auto& mappings = kind == ResourceKind::Sampler ? sampler_slots : resource_slots;
    Require(bind_index < mappings.size(), "missing captured reflection slot");
    return mappings[bind_index];
}

StageBindings::StageBindings(HardwareStage stage) : stage_(stage) {
    Require(static_cast<std::size_t>(stage) < UserBases.size(), "unknown hardware stage");
}

void StageBindings::SeedStaging(std::span<const std::uint32_t> prefix) {
    Require(prefix.size() <= shadow_.size(), "captured staging prefix too large");
    for (std::size_t i = 0; i < prefix.size(); ++i) shadow_[i] = prefix[i];
    // Seeding diagnostic state does not invent a dirty event.
}

void StageBindings::Bind(Backend& backend, std::uint16_t mapping, const Descriptor& descriptor) {
    const auto words = descriptor.Words();
    const auto location = BindingLocation::Decode(mapping);
    if (location.route == BindingRoute::Table) {
        Require(location.index + words.size() <= shadow_.size(), "descriptor outside stage table");
        for (std::size_t i = 0; i < words.size(); ++i) shadow_[location.index + i] = words[i];
    } else if (location.route == BindingRoute::UserSgpr) {
        EmitUserData(backend, stage_, location.index, words, true);
    }
    dirty_ = true; // Includes unused and direct mappings, matching observed helpers.
}

std::optional<TableUpload> StageBindings::CommitCompute(Backend& backend,
                                                       const ComputeReflection& reflection) {
    Require(stage_ == HardwareStage::CS, "graphics LCUE commit is not reconstructed");
    Require(!reflection.has_unmodeled_extra_bindings, "reflection extras require another adapter");
    if (!dirty_) return std::nullopt;
    Require(reflection.table_dwords <= shadow_.size(), "table extent exceeds stage storage");
    TableUpload upload;
    upload.words.reserve(reflection.table_dwords);
    for (std::size_t i = 0; i < reflection.table_dwords; ++i) {
        // Python inspection model allows None. This emitter refuses to invent GPU data.
        Require(shadow_[i].has_value(), "incomplete staging capture; seed missing words first");
        upload.words.push_back(*shadow_[i]);
    }
    upload.address = backend.AllocateUpload(UploadKind::LcueTable, reflection.table_dwords * 4u, 4);
    backend.WriteUpload(upload.address, std::as_bytes(std::span<const std::uint32_t>(upload.words)));
    for (std::size_t i = 0; i < reflection.pointer_sgprs.size(); ++i) {
        if (reflection.pointer_sgprs[i] == 255) continue;
        const auto displacement = i == 0 ? 0u : 4u * reflection.subtable_dwords[i - 1];
        const auto pointer = upload.address + displacement;
        const std::array<std::uint32_t, 2> words{static_cast<std::uint32_t>(pointer),
                                               static_cast<std::uint32_t>(pointer >> 32)};
        EmitUserData(backend, stage_, reflection.pointer_sgprs[i], words, false);
    }
    dirty_ = false;
    return upload;
}

void CommitResourceTables(Backend& backend, ResourceResolver& resolver,
                          const ShaderResourceTable& tables, const ComputeReflection& reflection,
                          UniformBindings& bindings, StageBindings& stage) {
    auto active = tables.uniform_mask & bindings.dirty_mask;
    while (active) {
        const auto slot = static_cast<std::size_t>(std::countr_zero(active));
        const auto* uniform = bindings.buffers[slot];
        Require(uniform != nullptr, "active UB slot has no captured uniform buffer");
        for (std::size_t kind_index = 0; kind_index < tables.maps.size(); ++kind_index) {
            const auto& map = tables.maps[kind_index];
            if (map.empty()) continue; // Caller explicitly supplies an absent resource map.
            Require(slot < map.size(), "missing packed-map prefix");
            std::size_t index = map[slot];
            if (index == 0) continue;
            for (;;) {
                Require(index < map.size(), "packed-map capture lacks next-slot/sentinel");
                const auto entry = ResourceEntry::Decode(map[index++]);
                if (entry.uniform_slot != slot) break;
                Require(entry.resource_index < uniform->resources.size(), "UB resource index out of range");
                const auto kind = static_cast<ResourceKind>(kind_index);
                const auto descriptor = resolver.Resolve(kind, uniform->resources[entry.resource_index]);
                stage.Bind(backend, reflection.Slot(kind, entry.bind_index), descriptor);
            }
        }
        active &= active - 1;
    }
    bindings.dirty_mask = 0; // Entire stage mask, including unused bits.
}

BufferDescriptor RefreshBufferSrv(BufferDescriptor descriptor, GpuAddress current_backing) {
    Require(current_backing < (1ull << 44), "SRV backing exceeds observed address width");
    descriptor[0] = static_cast<std::uint32_t>(current_backing);
    descriptor[1] = (descriptor[1] & 0xfffff000) |
                    static_cast<std::uint32_t>((current_backing >> 32) & 0xfff);
    return descriptor;
}

void FlushPriorGraphics(Backend& backend, std::vector<FlushRange>& ranges) {
    const auto label = NewLabel(backend);
    Emit(backend, {0xc0044700, 0x38504, static_cast<std::uint32_t>(label) & ~7u,
                   static_cast<std::uint32_t>(label >> 32) | 0x40000000, 1, 0});
    WaitForLabel(backend, label);
    for (const auto& range : ranges) {
        Require(range.base < (1ull << 40), "ranged acquire would truncate base");
        Emit(backend, {0xc0055800, 0x02003fc0, range.bytes >> 8, 0,
                       static_cast<std::uint32_t>(range.base >> 8), 0, 10});
    }
    AcquireComputeCaches(backend);
    ranges.clear();
}

void FlushAfterCompute(Backend& backend) {
    const auto label = NewLabel(backend);
    Emit(backend, {0xc0034802, 0x62f, static_cast<std::uint32_t>(label) & ~3u,
                   static_cast<std::uint32_t>(label >> 32) | 0x40000000, 1});
    WaitForLabel(backend, label);
    AcquireComputeCaches(backend);
}

GnmRhi::GnmRhi(Backend& backend, ResourceResolver& resolver) : backend_(backend), resolver_(resolver) {}

void GnmRhi::PrepareCompute(ComputeContext& context, std::optional<GpuAddress> indirect_base) {
    Require(context.shader != nullptr, "no compute shader supplied");
    const auto& shader = *context.shader;
    Require(!shader.reflection.has_unmodeled_extra_bindings, "unsupported reflection extras");
    if (context.auto_flush) FlushPriorGraphics(backend_, context.pending_graphics_ranges);
    if (shader.uses_loose_constants) {
        Require(context.loose != nullptr, "shader requires loose constant shadow");
        if (auto upload = context.loose->Commit(backend_, true)) {
            context.descriptors.Bind(backend_, shader.reflection.constant_slot0,
                                     Descriptor::Buffer(upload->descriptor));
            context.loose->AcknowledgeBinding();
        }
    }
    CommitResourceTables(backend_, resolver_, shader.resources, shader.reflection,
                          context.uniforms, context.descriptors);
    if (indirect_base) {
        Require(*indirect_base < (1ull << 40) && (*indirect_base & 7) == 0,
                "indirect base must fit observed aligned 40-bit address");
        Emit(backend_, {0xc0004200, 0}); // PFP_SYNC_ME before SET_BASE in 0x3772d0.
        Emit(backend_, {0xc0021102, 1, static_cast<std::uint32_t>(*indirect_base),
                       static_cast<std::uint32_t>(*indirect_base >> 32)});
    }
    if (context.shader_dirty) backend_.BindComputeShader(shader.backend_token);
    context.descriptors.CommitCompute(backend_, shader.reflection);
    context.shader_dirty = false;
}

void GnmRhi::FinishCompute(ComputeContext& context) {
    if (context.auto_flush && context.post_compute_flush) FlushAfterCompute(backend_);
    // Other KH3 texture/state bookkeeping after this point is not reconstructed.
}

void GnmRhi::DispatchCompute(ComputeContext& context, std::uint32_t x,
                            std::uint32_t y, std::uint32_t z) {
    PrepareCompute(context);
    backend_.DispatchDirect(x, y, z, context.dispatch_flags);
    FinishCompute(context);
}

void GnmRhi::DispatchComputeIndirect(ComputeContext& context, GpuAddress backing,
                                    std::uint32_t offset) {
    PrepareCompute(context, backing);
    backend_.DispatchIndirect(offset, context.dispatch_flags);
    FinishCompute(context);
}

std::size_t BufferSlotCount(std::uint32_t usage) {
    if (usage & Volatile) return 1;
    if (usage & (Static | Uav)) return 1;
    if (usage & Dynamic) return 3;
    throw std::invalid_argument("buffer usage outside reconstructed constructor cases");
}

MultiBuffer::MultiBuffer(std::uint32_t usage, std::uint32_t allocation_size,
                         std::vector<GpuAddress> existing_slots, std::size_t current_slot)
    : usage_(usage), allocation_size_(allocation_size), slots_(std::move(existing_slots)),
      current_slot_(current_slot) {
    Require(slots_.size() == BufferSlotCount(usage), "slot count disagrees with KH3 usage policy");
    Require(current_slot_ < slots_.size(), "current slot outside supplied backing array");
}

GpuAddress MultiBuffer::Lock(std::uint32_t raw_mode, std::uint32_t offset,
                             const std::function<GpuAddress(std::uint32_t)>& allocate_transient) {
    Require(offset <= allocation_size_, "lock offset outside modeled allocation");
    if (raw_mode != 0) {
        if (usage_ & Volatile) slots_[0] = allocate_transient(allocation_size_);
        else current_slot_ = (current_slot_ + 1) % slots_.size();
    }
    return CurrentData() + offset;
}

GpuAddress MultiBuffer::CurrentData() const {
    return slots_[(usage_ & Volatile) ? 0 : current_slot_];
}

void DeferredReleaseQueue::Enqueue(ResourceHandle allocation) {
    entries_.push_back({ticket_, allocation});
}

std::uint64_t DeferredReleaseQueue::RetireAndAdvance(
    std::uint64_t completed_ticket, const std::function<void(ResourceHandle)>& release) {
    Require(ticket_ != std::numeric_limits<std::uint64_t>::max(), "ticket wrap not modeled");
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->ticket <= completed_ticket) {
            release(it->allocation);
            it = entries_.erase(it);
        } else ++it;
    }
    return ticket_++; // The old ticket is what the frame-end code can emit into an EOP.
}

std::optional<std::uint64_t> ReadOcclusion(std::span<const OcclusionPair> pairs, bool neo) {
    const std::size_t count = neo ? 16 : 8;
    Require(pairs.size() >= count, "occlusion capture shorter than target backend count");
    std::uint64_t sum = 0;
    for (const auto& pair : pairs.first(count)) {
        if ((pair.begin & pair.end & (1ull << 63)) == 0) return std::nullopt;
        sum += pair.end - pair.begin;
    }
    return sum;
}

std::optional<std::uint64_t> ReadTimestamp(std::uint64_t timestamp, bool neo) {
    if (timestamp == 0) return std::nullopt;
    return timestamp / (neo ? 911u : 800u);
}

} // namespace kh3::gnm
