#include "recording_backend.h"

#include <iomanip>
#include <iostream>

using namespace kh3::gnm;
using namespace kh3::gnm::example;

int main() {
    RecordingBackend backend;
    CapturedResources resources;
    GnmRhi rhi(backend, resources);
    LooseConstants constants(std::vector<std::byte>(64)); // Explicit demo initialization.
    ComputeShader shader;
    shader.backend_token = 0x123;
    shader.uses_loose_constants = true;
    shader.reflection.table_dwords = 8;
    shader.reflection.constant_slot0 = 4;
    shader.reflection.pointer_sgprs[0] = 2;
    ComputeContext context;
    context.shader = &shader;
    context.loose = &constants;
    context.shader_dirty = true;
    context.auto_flush = context.post_compute_flush = true;
    context.dispatch_flags = 1;
    context.descriptors.SeedStaging(std::array<std::uint32_t, 8>{});
    const std::array<std::byte, 4> first{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 4> second{std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    constants.Write(32, first);
    rhi.DispatchCompute(context, 8, 4, 1);
    constants.Write(0, second);
    rhi.DispatchCompute(context, 8, 4, 1);

    std::cout << "KH3 01.10 GnmRHI semantic trace (synthetic addresses, no GPU execution)\n";
    for (const auto& event : backend.events) {
        std::cout << event.name;
        if (event.address) std::cout << " @0x" << std::hex << event.address;
        for (const auto word : event.words)
            std::cout << " " << std::hex << std::setw(8) << std::setfill('0') << word;
        std::cout << "\n";
    }
    std::cout << "Independent table snapshots:\n";
    for (const auto& table : backend.Tables()) {
        std::cout << "table @0x" << std::hex << table.address;
        for (auto word : backend.ReadWords(table.address, table.bytes.size() / 4))
            std::cout << " " << std::setw(8) << word;
        std::cout << "\n";
    }
}
