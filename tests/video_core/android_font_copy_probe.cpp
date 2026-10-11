// SPDX-License-Identifier: GPL-2.0-or-later
// Execute a captured font-copy compute shader with known inputs on Android.
#include <bit>
#include <cstdio>
#include <fstream>
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
std::vector<u32> Read(const char* name) {
    std::ifstream in(name, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error(name);
    const auto size = in.tellg();
    if (size <= 0 || size % 4)
        throw std::runtime_error("fixture size");
    std::vector<u32> result(size / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(result.data()), size);
    if (!in)
        throw std::runtime_error("fixture read");
    return result;
}
// Usage: hook-directory driver-directory captured-copy.spv [push|set|copy]
int Run(int argc, char** argv) {
    if (argc != 5 || (std::string_view(argv[4]) != "push" && std::string_view(argv[4]) != "set" &&
                      std::string_view(argv[4]) != "copy")) {
        fprintf(stderr,
                "Usage: %s hook-directory driver-directory captured-copy.spv push|set|copy\n",
                argv[0]);
        return 2;
    }
    const bool push = std::string_view(argv[4]) == "push";
    const bool transfer = std::string_view(argv[4]) == "copy";
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("font-copy-probe");
    Window window;
    const auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    const auto d = instance.GetDevice();
    const auto props = instance.GetMemoryProperties();
    auto allocate = [&](vk::MemoryRequirements r, vk::MemoryPropertyFlags flags) {
        for (u32 i = 0; i < props.memoryTypeCount; ++i)
            if ((r.memoryTypeBits & (1U << i)) &&
                (props.memoryTypes[i].propertyFlags & flags) == flags)
                return Vulkan::Check(
                    d.allocateMemoryUnique({.allocationSize = r.size, .memoryTypeIndex = i}));
        throw std::runtime_error("memory type");
    };
    constexpr u32 Pixels = 1024 * 1024, Readback = Pixels, Controls = Pixels * 2;
    // Memory owners are declared first so buffers/images are destroyed before their memory.
    vk::UniqueDeviceMemory memory;
    auto buffer = Vulkan::Check(d.createBufferUnique(
        {.size = Controls + 4096,
         .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                  vk::BufferUsageFlagBits::eTransferDst}));
    memory = allocate(d.getBufferMemoryRequirements(*buffer),
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent);
    Vulkan::Check(d.bindBufferMemory(*buffer, *memory, 0));
    auto* data = static_cast<u8*>(Vulkan::Check(d.mapMemory(*memory, 0, Controls + 4096)));
    std::array<vk::UniqueDeviceMemory, 2> image_memory;
    std::array<vk::UniqueImage, 2> images;
    std::array<vk::UniqueImageView, 2> views;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    for (u32 i = 0; i < 2; ++i) {
        images[i] = Vulkan::Check(d.createImageUnique(
            {.flags = vk::ImageCreateFlagBits::eMutableFormat,
             .imageType = vk::ImageType::e2D,
             .format = vk::Format::eR8Unorm,
             .extent = {1024, 1024, 1},
             .mipLevels = 1,
             .arrayLayers = 1,
             .samples = vk::SampleCountFlagBits::e1,
             .tiling = vk::ImageTiling::eOptimal,
             .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                      vk::ImageUsageFlagBits::eTransferSrc |
                      vk::ImageUsageFlagBits::eTransferDst}));
        image_memory[i] = allocate(d.getImageMemoryRequirements(*images[i]), {});
        Vulkan::Check(d.bindImageMemory(*images[i], *image_memory[i], 0));
        views[i] = Vulkan::Check(d.createImageViewUnique({.image = *images[i],
                                                          .viewType = vk::ImageViewType::e2D,
                                                          .format = vk::Format::eR8Unorm,
                                                          .subresourceRange = range}));
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{
        {{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
         {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
         {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
         {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute}}};
    auto layout = Vulkan::Check(d.createDescriptorSetLayoutUnique(
        {.flags = push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                       : vk::DescriptorSetLayoutCreateFlags{},
         .bindingCount = 4,
         .pBindings = bindings.data()}));
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, 128};
    auto pl = Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                          .pSetLayouts = &*layout,
                                                          .pushConstantRangeCount = 1,
                                                          .pPushConstantRanges = &push_range}));
    const std::array<vk::DescriptorPoolSize, 3> sizes{{{vk::DescriptorType::eStorageBuffer, 2},
                                                       {vk::DescriptorType::eSampledImage, 1},
                                                       {vk::DescriptorType::eStorageImage, 1}}};
    auto pool = Vulkan::Check(d.createDescriptorPoolUnique(
        {.maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes.data()}));
    vk::DescriptorSet set{};
    if (!push)
        set = Vulkan::Check(d.allocateDescriptorSets(
            {.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}))[0];
    const auto code = Read(argv[3]);
    auto module = Vulkan::Check(
        d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize = 64};
    auto pipeline = Vulkan::Check(d.createComputePipelineUnique(
        {}, {.stage = {.pNext = instance.IsSubgroupSize64Supported() ? &subgroup : nullptr,
                       .stage = vk::ShaderStageFlagBits::eCompute,
                       .module = *module,
                       .pName = "main"},
             .layout = *pl}));
    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto cmd = Vulkan::Check(d.allocateCommandBuffers({.commandPool = *cp,
                                                       .level = vk::CommandBufferLevel::ePrimary,
                                                       .commandBufferCount = 1}))[0];
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    const vk::BufferImageCopy copy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .imageExtent = {1024, 1024, 1}};
    auto readback = copy;
    readback.bufferOffset = Readback;
    unsigned failures = 0;
    for (u32 iteration = 0; iteration < 32; ++iteration) {
        for (u32 i = 0; i < Pixels; ++i)
            data[i] = u8((i * 17 + (i / 1024) * 31 + iteration * 13) % 256);
        std::memset(data + Readback, 0xa5, Pixels + 4096);
        const std::array<u32, 4> rect{0, 0, 1024, 1024};
        std::memcpy(data + Controls, rect.data(), 16);
        // The source T# dimensions are consumed only for scaling (disabled in push data).
        const std::array<u32, 8> flat{0, 0, 1023 | (1023 << 14), 0, 0, 0, 0, 0};
        std::memcpy(data + Controls + 256, flat.data(), sizeof(flat));
        const std::array<vk::DescriptorBufferInfo, 2> bi{
            {{*buffer, Controls, 16}, {*buffer, Controls + 256, sizeof(flat)}}};
        const std::array<vk::DescriptorImageInfo, 2> ii{
            {{{}, *views[0], vk::ImageLayout::eShaderReadOnlyOptimal},
             {{}, *views[1], vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 4> writes{};
        for (u32 i = 0; i < 4; ++i)
            writes[i] = {.dstSet = set,
                         .dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = bindings[i].descriptorType,
                         .pImageInfo = i >= 2 ? &ii[i - 2] : nullptr,
                         .pBufferInfo = i < 2 ? &bi[i] : nullptr};
        if (!push)
            d.updateDescriptorSets(writes, {});
        Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
        auto transition = [&](u32 image, vk::ImageLayout old, vk::ImageLayout next,
                              vk::AccessFlags src, vk::AccessFlags dst) {
            cmd.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                {}, {}, {},
                vk::ImageMemoryBarrier{.srcAccessMask = src,
                                       .dstAccessMask = dst,
                                       .oldLayout = old,
                                       .newLayout = next,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = *images[image],
                                       .subresourceRange = range});
        };
        transition(
            0, iteration ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            iteration ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{},
            vk::AccessFlagBits::eTransferWrite);
        transition(
            1, iteration ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral,
            iteration ? vk::AccessFlagBits::eTransferRead : vk::AccessFlags{},
            transfer ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eShaderWrite);
        cmd.copyBufferToImage(*buffer, *images[0], vk::ImageLayout::eTransferDstOptimal, copy);
        transition(0, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                   vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead);
        std::array<u32, 32> constants{};
        cmd.pushConstants(*pl, vk::ShaderStageFlagBits::eCompute, 0, sizeof(constants),
                          constants.data());
        if (push)
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl, 0, writes);
        else
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pl, 0, set, {});
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        if (transfer) {
            transition(0, vk::ImageLayout::eShaderReadOnlyOptimal,
                       vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits::eShaderRead,
                       vk::AccessFlagBits::eTransferRead);
            cmd.copyImage(
                *images[0], vk::ImageLayout::eTransferSrcOptimal, *images[1],
                vk::ImageLayout::eGeneral,
                vk::ImageCopy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                              .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                              .extent = {1024, 1024, 1}});
            transition(0, vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eTransferRead,
                       vk::AccessFlagBits::eShaderRead);
        } else {
            cmd.dispatch(128, 128, 1);
        }
        transition(1, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                   transfer ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eShaderWrite,
                   vk::AccessFlagBits::eTransferRead);
        cmd.copyImageToBuffer(*images[1], vk::ImageLayout::eTransferSrcOptimal, *buffer, readback);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {},
                            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                              .dstAccessMask = vk::AccessFlagBits::eHostRead},
                            {}, {});
        Vulkan::Check(cmd.end());
        Vulkan::Check(instance.GetGraphicsQueue().submit(
            vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, *fence));
        if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
            throw std::runtime_error("GPU timeout");
        u32 mismatch = 0;
        for (u32 i = 0; i < Pixels; ++i)
            mismatch += data[i] != data[Readback + i];
        printf("iteration=%u mismatched_bytes=%u first=%u expected=%u\n", iteration, mismatch,
               data[Readback], data[0]);
        failures += mismatch != 0;
        if (iteration == 0) {
            std::ofstream out(std::string(argv[4]) + "-output.bin", std::ios::binary);
            out.write(reinterpret_cast<char*>(data + Readback), Pixels);
            u32 shown = 0;
            for (u32 i = 0; i < Pixels && shown < 16; ++i)
                if (data[i] != data[Readback + i]) {
                    printf("mismatch x=%u y=%u actual=%u expected=%u\n", i % 1024, i / 1024,
                           data[Readback + i], data[i]);
                    ++shown;
                }
        }
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    }
    d.unmapMemory(*memory);
    printf("FONT_COPY mode=%s iterations=32 failures=%u\n", argv[4], failures);
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
