#pragma once

// Deterministic CPU-only adapter for examples/tests. No real GPU execution.
#include "gnm_rhi_semantic.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace kh3::gnm::example {

struct Event {
    std::string name;
    GpuAddress address{};
    std::vector<std::uint32_t> words;
};
struct Allocation {
    GpuAddress address{};
    std::string kind;
    std::vector<std::byte> bytes;
};

class RecordingBackend final : public Backend {
public:
    std::vector<Event> events;
    std::vector<Allocation> allocations;

    GpuAddress AllocateUpload(UploadKind kind, std::uint32_t bytes,
                             std::uint32_t alignment) override {
        const char* name = kind == UploadKind::LcueTable ? "table" :
                           kind == UploadKind::TransientConstants ? "transient-constants" : "constants";
        return Allocate(name, bytes, alignment);
    }
    void WriteUpload(GpuAddress address, std::span<const std::byte> bytes) override {
        auto& allocation = Find(address, bytes.size());
        std::copy(bytes.begin(), bytes.end(), allocation.bytes.begin() + (address - allocation.address));
        events.push_back({"cpu-write", address, {static_cast<std::uint32_t>(bytes.size())}});
    }
    void EmitPacket(std::span<const std::uint32_t> words) override {
        events.push_back({"packet", 0, {words.begin(), words.end()}});
    }
    GpuAddress ReserveZeroedLabel8() override {
        // A real backend must embed NOP storage in the command stream. The recorder
        // has a separate synthetic label allocation and records that abstraction.
        const auto address = Allocate("embedded-label-model", 8, 8);
        const std::array<std::byte, 8> zeros{};
        WriteUpload(address, zeros);
        return address;
    }
    void BindComputeShader(std::uint64_t shader_token) override {
        events.push_back({"sdk-bind-cs", shader_token, {}});
    }
    void DispatchDirect(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                        std::uint32_t flags) override {
        events.push_back({"sdk-dispatch-direct", 0, {x, y, z, flags}});
    }
    void DispatchIndirect(std::uint32_t offset, std::uint32_t flags) override {
        events.push_back({"sdk-dispatch-indirect", 0, {offset, flags}});
    }
    std::vector<std::uint32_t> ReadWords(GpuAddress address, std::size_t count) {
        auto& allocation = Find(address, count * 4);
        std::vector<std::uint32_t> result(count);
        if (count) std::memcpy(result.data(), allocation.bytes.data() + (address - allocation.address), count * 4);
        return result;
    }
    std::vector<Allocation> Tables() const {
        std::vector<Allocation> result;
        for (const auto& allocation : allocations)
            if (allocation.kind == "table") result.push_back(allocation);
        return result;
    }

private:
    GpuAddress next_{0x100000000ull};
    GpuAddress Allocate(const char* kind, std::uint32_t bytes, std::uint32_t alignment) {
        const auto address = (next_ + alignment - 1) & ~(GpuAddress(alignment) - 1);
        next_ = address + std::max(bytes, 1u);
        // Poison reveals an accidental assumption that upload padding was zeroed.
        allocations.push_back({address, kind, std::vector<std::byte>(bytes, std::byte{0xcd})});
        events.push_back({std::string("allocate-") + kind, address, {bytes}});
        return address;
    }
    Allocation& Find(GpuAddress address, std::size_t bytes) {
        for (auto& allocation : allocations) {
            if (address >= allocation.address && address - allocation.address <= allocation.bytes.size() &&
                bytes <= allocation.bytes.size() - (address - allocation.address)) return allocation;
        }
        throw std::out_of_range("synthetic address outside allocation");
    }
};

class CapturedResources final : public ResourceResolver {
public:
    std::unordered_map<ResourceHandle, Descriptor> descriptors;
    std::vector<std::pair<ResourceKind, ResourceHandle>> accesses;
    Descriptor Resolve(ResourceKind kind, ResourceHandle handle) override {
        accesses.emplace_back(kind, handle);
        return descriptors.at(handle);
    }
};

} // namespace kh3::gnm::example
