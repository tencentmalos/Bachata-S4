// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the full resource-pass fixtures emitted by srt_tests on the selected driver.
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
    return result;
}
int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("dynamic-image-probe");
    Window window;
    const auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    const auto d = instance.GetDevice();
    const auto memory_props = instance.GetMemoryProperties();
    auto allocate = [&](const vk::MemoryRequirements& r, vk::MemoryPropertyFlags flags) {
        for (u32 i = 0; i < memory_props.memoryTypeCount; ++i) {
            if ((r.memoryTypeBits & (1U << i)) &&
                (memory_props.memoryTypes[i].propertyFlags & flags) == flags)
                return Vulkan::Check(
                    d.allocateMemoryUnique({.allocationSize = r.size, .memoryTypeIndex = i}));
        }
        throw std::runtime_error("memory type");
    };
    auto buffer = Vulkan::Check(
        d.createBufferUnique({.size = 8192, .usage = vk::BufferUsageFlagBits::eStorageBuffer}));
    auto memory = allocate(d.getBufferMemoryRequirements(*buffer),
                           vk::MemoryPropertyFlagBits::eHostVisible |
                               vk::MemoryPropertyFlagBits::eHostCoherent);
    Vulkan::Check(d.bindBufferMemory(*buffer, *memory, 0));
    auto* data = static_cast<u32*>(Vulkan::Check(d.mapMemory(*memory, 0, 8192)));
    std::array<vk::UniqueImage, 2> images;
    std::array<vk::UniqueDeviceMemory, 2> image_memory;
    std::array<vk::UniqueImageView, 2> views;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    for (u32 i = 0; i < 2; ++i) {
        images[i] = Vulkan::Check(d.createImageUnique(
            {.imageType = vk::ImageType::e2D,
             .format = vk::Format::eR8G8B8A8Unorm,
             .extent = {32, 32, 1},
             .mipLevels = 1,
             .arrayLayers = 1,
             .samples = vk::SampleCountFlagBits::e1,
             .tiling = vk::ImageTiling::eOptimal,
             .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst}));
        image_memory[i] = allocate(d.getImageMemoryRequirements(*images[i]), {});
        Vulkan::Check(d.bindImageMemory(*images[i], *image_memory[i], 0));
        views[i] = Vulkan::Check(d.createImageViewUnique({.image = *images[i],
                                                          .viewType = vk::ImageViewType::e2D,
                                                          .format = vk::Format::eR8G8B8A8Unorm,
                                                          .subresourceRange = range}));
    }
    auto sampler = Vulkan::Check(d.createSamplerUnique(
        {.magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest, .maxLod = 1.f}));
    const auto layout_counts = Read("dynamic-image-layout.bin");
    if (layout_counts.size() != 3 || layout_counts[0] != 2 || layout_counts[2] != 1 ||
        layout_counts[1] < 2 || layout_counts[1] > 32)
        throw std::runtime_error("unexpected fixture binding layout");
    const u32 sampler_binding = 2 + layout_counts[1];
    const u32 binding_count = sampler_binding + 1;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (u32 i = 0; i < binding_count; ++i)
        bindings.push_back({.binding = i,
                            .descriptorType = i < 2 ? vk::DescriptorType::eStorageBuffer
                                              : i == sampler_binding
                                                  ? vk::DescriptorType::eSampler
                                                  : vk::DescriptorType::eSampledImage,
                            .descriptorCount = 1,
                            .stageFlags = vk::ShaderStageFlagBits::eCompute});
    auto layout = Vulkan::Check(d.createDescriptorSetLayoutUnique(
        {.bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, 128};
    auto pipeline_layout =
        Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                    .pSetLayouts = &*layout,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &push_range}));
    const std::array<vk::DescriptorPoolSize, 3> counts{
        {{vk::DescriptorType::eStorageBuffer, 2},
         {vk::DescriptorType::eSampledImage, layout_counts[1]},
         {vk::DescriptorType::eSampler, 1}}};
    auto pool = Vulkan::Check(d.createDescriptorPoolUnique(
        {.maxSets = 1, .poolSizeCount = 3, .pPoolSizes = counts.data()}));
    const auto sets = Vulkan::Check(d.allocateDescriptorSets(
        {.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}));
    auto flat = Read("dynamic-image-flat.bin");
    const std::array<vk::DescriptorBufferInfo, 2> bi{
        {{*buffer, 0, 1920}, {*buffer, 4096, flat.size() * 4}}};
    for (u32 i = 0; i < binding_count; ++i) {
        vk::DescriptorImageInfo ii{.sampler = *sampler,
                                   .imageView = *views[i == 3 ? 1 : 0],
                                   .imageLayout = vk::ImageLayout::eGeneral};
        d.updateDescriptorSets(vk::WriteDescriptorSet{.dstSet = sets[0],
                                                      .dstBinding = i,
                                                      .descriptorCount = 1,
                                                      .descriptorType = bindings[i].descriptorType,
                                                      .pImageInfo = i >= 2 ? &ii : nullptr,
                                                      .pBufferInfo = i < 2 ? &bi[i] : nullptr},
                               {});
    }
    auto command_pool = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto cmds = Vulkan::Check(d.allocateCommandBuffers({.commandPool = *command_pool,
                                                        .level = vk::CommandBufferLevel::ePrimary,
                                                        .commandBufferCount = 1}));
    auto cmd = cmds[0];
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    auto submit = [&] {
        Vulkan::Check(cmd.end());
        Vulkan::Check(instance.GetGraphicsQueue().submit(
            vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, *fence));
        if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
            throw std::runtime_error("GPU timeout");
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*command_pool));
    };
    Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
    for (u32 i = 0; i < 2; ++i) {
        vk::ImageMemoryBarrier barrier{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                                       .oldLayout = vk::ImageLayout::eUndefined,
                                       .newLayout = vk::ImageLayout::eGeneral,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = *images[i],
                                       .subresourceRange = range};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
        const std::array<float, 4> color =
            i ? std::array<float, 4>{0, 1, 0, 1} : std::array<float, 4>{1, 0, 0, 1};
        cmd.clearColorImage(*images[i], vk::ImageLayout::eGeneral, vk::ClearColorValue(color),
                            range);
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, barrier);
    }
    submit();
    const auto heap = Read("dynamic-image-heap.bin");
    const auto user = Read("dynamic-image-user.bin");
    unsigned checks{}, failures{};
    for (const auto* name : {"dynamic-image-scalar.spv", "dynamic-image-lane.spv"}) {
        const auto code = Read(name);
        auto module = Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        auto pipeline = Vulkan::Check(
            d.createComputePipelineUnique({}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                                         .module = *module,
                                                         .pName = "main"},
                                               .layout = *pipeline_layout}));
        for (u32 index : {2U, 8U, 0U, 2U, 8U, 39U}) {
            std::memcpy(data, heap.data(), heap.size() * 4);
            data[0] = index;
            std::memcpy(data + 1024, flat.data(), flat.size() * 4);
            std::array<u32, 32> push{};
            std::copy_n(user.data(), 4, push.data() + 4);
            Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, sets, {});
            cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                              push.data());
            cmd.dispatch(1, 1, 1);
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eHost, {},
                                vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                                  .dstAccessMask = vk::AccessFlagBits::eHostRead},
                                {}, {});
            submit();
            const std::array<float, 4> expected = index == 2   ? std::array<float, 4>{1, 0, 0, 1}
                                                  : index == 8 ? std::array<float, 4>{0, 1, 0, 1}
                                                               : std::array<float, 4>{0, 0, 0, 0};
            for (u32 c = 0; c < 4; ++c) {
                ++checks;
                if (data[39 * 12 + c] != std::bit_cast<u32>(expected[c])) {
                    ++failures;
                    printf("FAIL %s index=%u c=%u got=%08x expected=%f\n", name, index, c,
                           data[39 * 12 + c], expected[c]);
                }
            }
        }
    }
    d.unmapMemory(*memory);
    printf("DYNAMIC_IMAGE_GPU checks=%u failures=%u\n", checks, failures);
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
