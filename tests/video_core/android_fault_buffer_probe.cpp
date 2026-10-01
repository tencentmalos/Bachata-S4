// SPDX-License-Identifier: GPL-2.0-or-later
// Run the production fault parser SPIR-V with a bounded overflow workload.
#include <bit>
#include <cstdio>
#include <fstream>
#include <set>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "video_core/renderer_vulkan/vk_instance.h"

struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 64;
    }
    s32 GetHeight() const override {
        return 64;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};

int Run(int argc, char** argv) {
    if (argc != 4)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("fault-buffer-probe");
    Window window;
    const auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    const auto d = instance.GetDevice();
    const auto props = instance.GetMemoryProperties();
    auto allocate = [&](const vk::MemoryRequirements& r) {
        const auto flags =
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for (u32 i = 0; i < props.memoryTypeCount; ++i)
            if ((r.memoryTypeBits & (1U << i)) &&
                (props.memoryTypes[i].propertyFlags & flags) == flags)
                return Vulkan::Check(
                    d.allocateMemoryUnique({.allocationSize = r.size, .memoryTypeIndex = i}));
        throw std::runtime_error("coherent memory type");
    };
    constexpr u32 words = 129, slots = 1024, guard_words = 16;
    std::array<vk::UniqueBuffer, 2> buffers;
    std::array<vk::UniqueDeviceMemory, 2> memory;
    std::array<u32*, 2> mapped{};
    const std::array<u32, 2> sizes{words * 4, slots * 8 + guard_words * 4};
    for (u32 i = 0; i < 2; ++i) {
        buffers[i] =
            Vulkan::Check(d.createBufferUnique({.size = sizes[i],
                                                .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                                                         vk::BufferUsageFlagBits::eTransferDst}));
        memory[i] = allocate(d.getBufferMemoryRequirements(*buffers[i]));
        Vulkan::Check(d.bindBufferMemory(*buffers[i], *memory[i], 0));
        mapped[i] = static_cast<u32*>(Vulkan::Check(d.mapMemory(*memory[i], 0, sizes[i])));
    }
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
    for (u32 i = 0; i < 2; ++i)
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto layout = Vulkan::Check(
        d.createDescriptorSetLayoutUnique({.bindingCount = 2, .pBindings = bindings.data()}));
    auto pipeline_layout =
        Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*layout}));
    const vk::DescriptorPoolSize count{vk::DescriptorType::eStorageBuffer, 2};
    auto pool = Vulkan::Check(
        d.createDescriptorPoolUnique({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &count}));
    auto sets = Vulkan::Check(d.allocateDescriptorSets(
        {.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}));
    for (u32 i = 0; i < 2; ++i) {
        const vk::DescriptorBufferInfo info{*buffers[i], 0, i == 0 ? words * 4 : slots * 8};
        d.updateDescriptorSets(
            vk::WriteDescriptorSet{.dstSet = sets[0],
                                   .dstBinding = i,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eStorageBuffer,
                                   .pBufferInfo = &info},
            {});
    }
    std::ifstream input(argv[3], std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size <= 0 || size % 4)
        throw std::runtime_error("SPIR-V input");
    std::vector<u32> code(size / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), size);
    auto module =
        Vulkan::Check(d.createShaderModuleUnique({.codeSize = size_t(size), .pCode = code.data()}));
    const std::array<u32, 2> spec_data{16, slots};
    const std::array<vk::SpecializationMapEntry, 2> entries{{{0, 0, 4}, {1, 4, 4}}};
    const vk::SpecializationInfo spec{.mapEntryCount = 2,
                                      .pMapEntries = entries.data(),
                                      .dataSize = sizeof(spec_data),
                                      .pData = spec_data.data()};
    auto pipeline = Vulkan::Check(
        d.createComputePipelineUnique({}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                                     .module = *module,
                                                     .pName = "main",
                                                     .pSpecializationInfo = &spec},
                                           .layout = *pipeline_layout}));
    auto command_pool = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    const auto commands =
        Vulkan::Check(d.allocateCommandBuffers({.commandPool = *command_pool,
                                                .level = vk::CommandBufferLevel::ePrimary,
                                                .commandBufferCount = 1}));
    const auto cmd = commands[0];
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    unsigned checks{}, failures{};
    const auto check = [&](bool ok, const char* message) {
        ++checks;
        if (!ok) {
            ++failures;
            if (failures < 20)
                printf("FAIL %s\n", message);
        }
    };
    for (u32 population : {0U, 1U, 1023U, 1024U, 2000U, 4096U}) {
        std::fill_n(mapped[0], words, 0U);
        std::set<u64> expected, received;
        for (u32 i = 0; i < population; ++i) {
            const u32 page = (i * 17) % (words * 32);
            mapped[0][page / 32] |= 1U << (page % 32);
            expected.insert(u64(page) << 16);
        }
        for (u32 round = 0; round < 6; ++round) {
            std::fill_n(mapped[1], sizes[1] / 4, 0x5a5a5a5aU);
            Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
            cmd.fillBuffer(*buffers[1], 0, 8, 0);
            cmd.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader, {},
                vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                  .dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                                   vk::AccessFlagBits::eShaderWrite},
                {}, {});
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, sets, {});
            cmd.dispatch((words + 63) / 64, 1, 1);
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eHost, {},
                                vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                                  .dstAccessMask = vk::AccessFlagBits::eHostRead},
                                {}, {});
            Vulkan::Check(cmd.end());
            Vulkan::Check(instance.GetGraphicsQueue().submit(
                vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, *fence));
            if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
                throw std::runtime_error("GPU timeout");
            Vulkan::Check(d.resetFences(*fence));
            Vulkan::Check(d.resetCommandPool(*command_pool));
            const u64* output = reinterpret_cast<const u64*>(mapped[1]);
            check(output[0] <= slots - 1, "reported count stays within allocation");
            for (u32 i = 1; i <= std::min<u64>(output[0], slots - 1); ++i) {
                check(expected.contains(output[i]), "reported page belongs to input");
                check(received.insert(output[i]).second, "page delivered exactly once");
            }
            auto outstanding = expected;
            for (const auto page : received)
                outstanding.erase(page);
            std::set<u64> remaining;
            for (u32 page = 0; page < words * 32; ++page)
                if (mapped[0][page / 32] & (1U << (page % 32)))
                    remaining.insert(u64(page) << 16);
            check(remaining == outstanding, "overflow bitmap retains every unreported page");
            for (u32 i = slots * 2; i < sizes[1] / 4; ++i)
                check(mapped[1][i] == 0x5a5a5a5aU, "output guard unchanged");
        }
        check(received == expected, "all pages eventually drained");
    }
    for (auto& m : memory)
        d.unmapMemory(*m);
    printf("FAULT_BUFFER_GPU checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    int result = 3;
    try {
        result = Run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
    }
    Common::Log::Shutdown();
    return result;
}
