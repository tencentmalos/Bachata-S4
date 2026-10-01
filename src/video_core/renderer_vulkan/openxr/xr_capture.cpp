// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include "video_core/renderer_vulkan/openxr/xr_capture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <stb_image_write.h>
#include <vk_mem_alloc.h>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "spatial/xr/XrSceneVulkanLayer.h"
#include "video_core/host_shaders/xr_capture_comp.h"
#include "video_core/renderer_vulkan/capture_recorder.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#define VKF(name) VULKAN_HPP_DEFAULT_DISPATCHER.vk##name

namespace Vulkan::OpenXr {
namespace {
// Mirrors the std140 Constants block of xr_capture.comp.
struct Vec4 {
    float v[4]{};
};
struct ProjectionUbo {
    Vec4 rotation[3];
    Vec4 tan_fov, rect, misc;
};
struct QuadUbo {
    Vec4 rotation[3];
    Vec4 center, size, rect, misc;
};
struct EyeUbo {
    Vec4 rotation[3];
    Vec4 tan_fov, position;
    ProjectionUbo environment;
    QuadUbo screen;
    ProjectionUbo game;
    ProjectionUbo status;
};
struct Constants {
    EyeUbo eyes[2];
    int32_t extent[4];
};
static_assert(sizeof(Constants) == 2 * 30 * 16 + 16);

using Matrix = std::array<std::array<float, 3>, 3>;
Matrix Rotation(const XrQuaternionf& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}
// Rows of R (local -> world) or of R^T (world -> local).
void Rows(Vec4 (&out)[3], const XrQuaternionf& q, bool inverse) {
    const auto r = Rotation(q);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i].v[j] = inverse ? r[j][i] : r[i][j];
}
Vec4 TanFov(const XrFovf& f) {
    return {{std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleDown),
             std::tan(f.angleUp)}};
}
Vec4 Rect(const XrRect2Di& r, uint32_t width, uint32_t height) {
    return {{float(r.offset.x) / width, float(r.offset.y) / height, float(r.extent.width) / width,
             float(r.extent.height) / height}};
}
VkFormat Unorm(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}
void CheckVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(int(result)));
}
bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t width,
              uint32_t height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) return false;
    struct Output {
        Common::FS::IOFile& file;
        bool ok{true};
    } output{file};
    auto callback = [](void* context, void* data, int size) {
        auto& out = *static_cast<Output*>(context);
        if (out.file.WriteRaw<uint8_t>(data, size) != static_cast<size_t>(size)) out.ok = false;
    };
    return stbi_write_png_to_func(callback, &output, int(width), int(height), 4, rgba.data(), 0) &&
           output.ok && file.Flush();
}
} // namespace

struct XrCapture::Impl {
    struct Image {
        VkImage image{};
        VmaAllocation allocation{};
        VkImageView view{};
        VkFormat format{};
        uint32_t width{}, height{}, layers{};
        bool initialized{}; // left UNDEFINED until first use
    };
    struct SceneSlot {
        Image image;
        spatial::xr::XrSceneVulkanLayer* attached{};
        uint64_t serial_at_attach{};
        bool valid{}; // submitted this frame and copied at least once into image
        std::array<XrCompositionLayerProjectionView, 2> views{};
    };

    explicit Impl(const Instance& instance) : vk(instance), recorder(instance, CaptureSource::Xr) {}
    ~Impl() {
        recorder.Close();
        const VkDevice device = vk.GetDevice();
        {
            std::scoped_lock lock(vk.QueueMutex());
            VKF(DeviceWaitIdle)(device);
        }
        for (auto* image : {&scenes[0].image, &scenes[1].image, &screen, &output, &dummy})
            DestroyImage(*image);
        if (readback) vmaDestroyBuffer(vk.GetAllocator(), readback, readback_allocation);
        if (constants) vmaDestroyBuffer(vk.GetAllocator(), constants, constants_allocation);
        if (pipeline) VKF(DestroyPipeline)(device, pipeline, nullptr);
        if (pipeline_layout) VKF(DestroyPipelineLayout)(device, pipeline_layout, nullptr);
        if (descriptor_pool) VKF(DestroyDescriptorPool)(device, descriptor_pool, nullptr);
        if (set_layout) VKF(DestroyDescriptorSetLayout)(device, set_layout, nullptr);
        if (sampler) VKF(DestroySampler)(device, sampler, nullptr);
        if (fence) VKF(DestroyFence)(device, fence, nullptr);
        if (pool) VKF(DestroyCommandPool)(device, pool, nullptr);
    }

    void DestroyImage(Image& image) {
        if (image.view) VKF(DestroyImageView)(vk.GetDevice(), image.view, nullptr);
        if (image.image) vmaDestroyImage(vk.GetAllocator(), image.image, image.allocation);
        image = {};
    }
    // Recreates `image` when its shape changes. Returns true when recreated.
    bool EnsureImage(Image& image, VkFormat format, VkFormat view_format, uint32_t width,
                     uint32_t height, uint32_t layers, VkImageUsageFlags usage) {
        if (image.image && image.format == format && image.width == width &&
            image.height == height && image.layers == layers)
            return false;
        if (image.image) {
            // Earlier frames' copies / reads of the old image must retire.
            std::scoped_lock lock(vk.QueueMutex());
            VKF(DeviceWaitIdle)(vk.GetDevice());
        }
        DestroyImage(image);
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = view_format != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        CheckVk(vmaCreateImage(vk.GetAllocator(), &info, &alloc, &image.image, &image.allocation,
                               nullptr),
                "XR capture image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = (usage & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_IMAGE_VIEW_TYPE_2D
                                                             : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = view_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        CheckVk(VKF(CreateImageView)(vk.GetDevice(), &view, nullptr, &image.view),
                "XR capture view");
        image.format = format;
        image.width = width;
        image.height = height;
        image.layers = layers;
        image.initialized = false;
        return true;
    }

    void InitPipeline() {
        if (pipeline) return;
        const VkDevice device = vk.GetDevice();
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = vk.GetGraphicsQueueFamilyIndex();
        CheckVk(VKF(CreateCommandPool)(device, &pool_info, nullptr, &pool), "XR capture pool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        CheckVk(VKF(AllocateCommandBuffers)(device, &alloc, &command), "XR capture command");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CheckVk(VKF(CreateFence)(device, &fence_info, nullptr, &fence), "XR capture fence");

        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.f;
        CheckVk(VKF(CreateSampler)(device, &sampler_info, nullptr, &sampler), "XR capture sampler");

        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 0   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_info.bindingCount = bindings.size();
        set_info.pBindings = bindings.data();
        CheckVk(VKF(CreateDescriptorSetLayout)(device, &set_info, nullptr, &set_layout),
                "XR capture set layout");
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        }};
        VkDescriptorPoolCreateInfo pool_create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_create.maxSets = 1;
        pool_create.poolSizeCount = sizes.size();
        pool_create.pPoolSizes = sizes.data();
        CheckVk(VKF(CreateDescriptorPool)(device, &pool_create, nullptr, &descriptor_pool),
                "XR capture descriptor pool");
        VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_alloc.descriptorPool = descriptor_pool;
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts = &set_layout;
        CheckVk(VKF(AllocateDescriptorSets)(device, &set_alloc, &set), "XR capture set");

        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout;
        CheckVk(VKF(CreatePipelineLayout)(device, &layout_info, nullptr, &pipeline_layout),
                "XR capture pipeline layout");
        const vk::ShaderModule module = CompileSPV(XR_CAPTURE_COMP, vk.GetDevice());
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = static_cast<VkShaderModule>(module);
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = pipeline_layout;
        const auto result =
            VKF(CreateComputePipelines)(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        VKF(DestroyShaderModule)(device, static_cast<VkShaderModule>(module), nullptr);
        CheckVk(result, "XR capture pipeline");

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = sizeof(Constants);
        buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo buffer_alloc{};
        buffer_alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        buffer_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        CheckVk(vmaCreateBuffer(vk.GetAllocator(), &buffer, &buffer_alloc, &constants,
                                &constants_allocation, &info),
                "XR capture constants");
        constants_mapped = info.pMappedData;
        // Stand-in for absent layers: never sampled (the slot is disabled), but
        // every binding must name an image in a valid layout.
        EnsureImage(dummy, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }

    void EnsureReadback(VkDeviceSize size) {
        if (readback && readback_size >= size) return;
        if (readback) vmaDestroyBuffer(vk.GetAllocator(), readback, readback_allocation);
        readback = {};
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        CheckVk(vmaCreateBuffer(vk.GetAllocator(), &buffer, &alloc, &readback, &readback_allocation,
                                &info),
                "XR capture readback");
        readback_mapped = info.pMappedData;
        readback_size = size;
    }

    void Barrier(VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED
                                    ? 0
                                    : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VKF(CmdPipelineBarrier)(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &barrier);
    }
    // A never-written image goes to SHADER_READ_ONLY (as black) before sampling.
    void Initialize(Image& image) {
        if (image.initialized) return;
        Barrier(image.image, image.layers, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.layers};
        VKF(CmdClearColorImage)(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                1, &range);
        Barrier(image.image, image.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image.initialized = true;
    }

    Frame RecorderFrame() const {
        Frame frame{};
        frame.width = output_width * 2;
        frame.height = output_height;
        frame.image = vk::Image{output.image};
        frame.xr_stereo = true;
        frame.id = 0;
        return frame;
    }

    const Instance& vk;
    CaptureRecorder recorder;
    std::array<SceneSlot, 2> scenes{};
    Image screen, output, dummy;
    bool screen_copied{};
    // Game layer of this frame: quads per eye, or the PSVR projection.
    std::array<std::optional<XrCompositionLayerQuad>, 2> screen_quads{};
    std::optional<std::array<XrCompositionLayerProjectionView, 2>> screen_projection;
    uint32_t output_width{}, output_height{};
    bool active{}, encoding{};
    std::optional<scrcpy::capture::SnapshotRequest> shot;
    uint64_t frames{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSampler sampler{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet set{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    VkBuffer constants{};
    VmaAllocation constants_allocation{};
    void* constants_mapped{};
    VkBuffer readback{};
    VmaAllocation readback_allocation{};
    void* readback_mapped{};
    VkDeviceSize readback_size{};
    bool failed{};
};

XrCapture::XrCapture(const Instance& vk) : impl(std::make_unique<Impl>(vk)) {}
XrCapture::~XrCapture() = default;

bool XrCapture::Active() const {
    return impl->active;
}

bool XrCapture::Begin(uint32_t eye_width, uint32_t eye_height) {
    auto& p = *impl;
    p.active = p.encoding = false;
    p.shot.reset();
    p.screen_copied = false;
    p.screen_quads = {};
    p.screen_projection.reset();
    for (auto& scene : p.scenes) scene.valid = false;
    if (p.failed) return false;
    // Half the runtime eye resolution: the capture is for review, not a
    // second full-resolution render.
    p.output_width = std::max(64u, (std::min(eye_width, 2592u) / 2) & ~1u);
    p.output_height = std::max(64u, (std::min(eye_height, 2400u) / 2) & ~1u);
    const bool selected = CurrentCaptureSource() == CaptureSource::Xr;
    try {
        if (selected) p.InitPipeline();
        // Always: picks up start/stop (and closes a recording after a stop).
        if (selected || p.output.image)
            p.recorder.SyncRequest(p.RecorderFrame(), vk::Format::eR8G8B8A8Unorm, 1);
        if (!selected) return false;
        p.shot = EmbeddedScreenshots().Take(p.vk.DiagnosticGeneration(), ++p.frames,
                                            Core::Diagnostics::DiagnosticNowNs());
        if (!p.shot && p.frames % 2) return false; // the encoder takes ~36 Hz of a 72 Hz loop
        p.EnsureImage(p.output, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                      p.output_width * 2, p.output_height, 1,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        p.encoding = p.recorder.Acquire(p.RecorderFrame());
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "XR capture disabled: {}", e.what());
        if (p.shot) EmbeddedScreenshots().Complete(p.shot->token, 0, 0, e.what());
        p.shot.reset();
        p.failed = true;
        return false;
    }
    p.active = p.shot.has_value() || p.encoding;
    return p.active;
}

void XrCapture::Attach(Scene scene, spatial::xr::XrSceneVulkanLayer& layer) {
    auto& p = *impl;
    auto& slot = p.scenes[static_cast<int>(scene)];
    if (!p.active) {
        layer.SetCaptureTarget(VK_NULL_HANDLE);
        slot.attached = nullptr;
        return;
    }
    try {
        const bool recreated = p.EnsureImage(
            slot.image, layer.Format(), Unorm(layer.Format()), layer.SwapchainWidth(),
            layer.EyeHeight(), layer.SwapchainLayers(),
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        // A serial below the recorded one = a new scene object at the same address.
        if (recreated || slot.attached != &layer ||
            layer.CaptureSerial() < slot.serial_at_attach) {
            slot.attached = &layer;
            slot.serial_at_attach = layer.CaptureSerial();
        }
        layer.SetCaptureTarget(slot.image.image);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "XR capture scene target: {}", e.what());
        layer.SetCaptureTarget(VK_NULL_HANDLE);
        slot.attached = nullptr;
    }
}

void XrCapture::Submitted(Scene scene, spatial::xr::XrSceneVulkanLayer& layer,
                          const XrCompositionLayerProjection& submitted) {
    auto& p = *impl;
    auto& slot = p.scenes[static_cast<int>(scene)];
    if (!p.active || slot.attached != &layer || submitted.viewCount < 2) return;
    // The Foundation copy precedes our compose on the same queue; until the
    // first copy into this image it holds nothing (and stays UNDEFINED).
    slot.valid = layer.CaptureSerial() != slot.serial_at_attach;
    if (slot.valid) {
        slot.image.initialized = true; // the copy left it SHADER_READ_ONLY
        for (uint32_t i = 0; i < 2; ++i) slot.views[i] = submitted.views[i];
    }
}

void XrCapture::CopyScreen(VkCommandBuffer cmd, VkImage mailbox, VkFormat format, uint32_t width,
                           uint32_t height) {
    auto& p = *impl;
    if (!p.active) return;
    try {
        p.EnsureImage(p.screen, format, Unorm(format), width, height, 1,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "XR capture screen: {}", e.what());
        return;
    }
    const auto previous = p.command;
    p.command = cmd; // Barrier() records into p.command
    p.Barrier(p.screen.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {width, height, 1};
    VKF(CmdCopyImage)(cmd, mailbox, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.screen.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    p.Barrier(p.screen.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    p.command = previous;
    p.screen.initialized = true;
    p.screen_copied = true;
}

void XrCapture::ScreenQuad(uint32_t eye_mask, const XrCompositionLayerQuad& quad) {
    auto& p = *impl;
    if (!p.active) return;
    for (uint32_t i = 0; i < 2; ++i)
        if (eye_mask & (1u << i)) p.screen_quads[i] = quad;
}

void XrCapture::ScreenProjection(const XrCompositionLayerProjection& projection) {
    auto& p = *impl;
    if (!p.active || projection.viewCount < 2) return;
    p.screen_projection = std::array{projection.views[0], projection.views[1]};
}

void XrCapture::Compose(const std::array<XrView, 2>& eyes) {
    auto& p = *impl;
    if (!p.active) return;
    p.active = false;
    std::string error;
    try {
        Constants constants{};
        constants.extent[0] = int32_t(p.output_width);
        constants.extent[1] = int32_t(p.output_height);
        constants.extent[2] = 2;
        const auto projection = [](ProjectionUbo& out, const XrCompositionLayerProjectionView& view,
                                   uint32_t width, uint32_t height) {
            Rows(out.rotation, view.pose.orientation, true);
            out.tan_fov = TanFov(view.fov);
            out.rect = Rect(view.subImage.imageRect, width, height);
            out.misc = {{float(view.subImage.imageArrayIndex), 1.f, 0.f, 0.f}};
        };
        for (uint32_t i = 0; i < 2; ++i) {
            auto& eye = constants.eyes[i];
            Rows(eye.rotation, eyes[i].pose.orientation, false);
            eye.tan_fov = TanFov(eyes[i].fov);
            eye.position = {{eyes[i].pose.position.x, eyes[i].pose.position.y,
                             eyes[i].pose.position.z, 0.f}};
            if (const auto& env = p.scenes[0]; env.valid)
                projection(eye.environment, env.views[i], env.image.width, env.image.height);
            if (const auto& status = p.scenes[1]; status.valid)
                projection(eye.status, status.views[i], status.image.width, status.image.height);
            if (p.screen_copied && p.screen_projection) {
                projection(eye.game, (*p.screen_projection)[i], p.screen.width, p.screen.height);
            } else if (p.screen_copied && p.screen_quads[i]) {
                const auto& quad = *p.screen_quads[i];
                Rows(eye.screen.rotation, quad.pose.orientation, true);
                eye.screen.center = {{quad.pose.position.x, quad.pose.position.y,
                                      quad.pose.position.z, 0.f}};
                eye.screen.size = {{quad.size.width, quad.size.height, 0.f, 0.f}};
                eye.screen.rect = Rect(quad.subImage.imageRect, p.screen.width, p.screen.height);
                eye.screen.misc = {{float(quad.subImage.imageArrayIndex), 1.f, 0.f, 0.f}};
            }
        }
        std::memcpy(p.constants_mapped, &constants, sizeof(constants));
        vmaFlushAllocation(p.vk.GetAllocator(), p.constants_allocation, 0, VK_WHOLE_SIZE);

        const VkDevice device = p.vk.GetDevice();
        CheckVk(VKF(ResetCommandPool)(device, p.pool, 0), "XR capture reset");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(VKF(BeginCommandBuffer)(p.command, &begin), "XR capture begin");
        p.Initialize(p.dummy);
        const auto sampled = [&](const Impl::Image& image, bool use) {
            const auto& source = use ? image : p.dummy;
            return VkDescriptorImageInfo{p.sampler, source.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        };
        const VkDescriptorBufferInfo buffer{p.constants, 0, sizeof(Constants)};
        const std::array images{sampled(p.scenes[0].image, p.scenes[0].valid),
                                sampled(p.screen, p.screen_copied),
                                sampled(p.scenes[1].image, p.scenes[1].valid)};
        const VkDescriptorImageInfo target{VK_NULL_HANDLE, p.output.view, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = p.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer;
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i + 1].pImageInfo = &images[i];
        }
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[4].pImageInfo = &target;
        // The previous compose's fence wait retired every use of the set.
        VKF(UpdateDescriptorSets)(device, writes.size(), writes.data(), 0, nullptr);

        p.Barrier(p.output.image, 1,
                  p.output.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL);
        p.output.initialized = true;
        VKF(CmdBindPipeline)(p.command, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        VKF(CmdBindDescriptorSets)(p.command, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0,
                                   1, &p.set, 0, nullptr);
        VKF(CmdDispatch)(p.command, (p.output_width * 2 + 7) / 8, (p.output_height + 7) / 8, 1);
        const uint32_t full_width = p.output_width * 2;
        if (p.shot) {
            p.EnsureReadback(VkDeviceSize(full_width) * p.output_height * 4);
            p.Barrier(p.output.image, 1, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {full_width, p.output_height, 1};
            VKF(CmdCopyImageToBuffer)(p.command, p.output.image,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.readback, 1, &copy);
            VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            to_host.srcQueueFamilyIndex = to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_host.buffer = p.readback;
            to_host.size = VK_WHOLE_SIZE;
            VKF(CmdPipelineBarrier)(p.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host, 0,
                                    nullptr);
            p.Barrier(p.output.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
            p.Barrier(p.output.image, 1, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        // The recorder blits its eye span from SHADER_READ_ONLY and puts it back.
        if (p.encoding) p.recorder.Record(vk::CommandBuffer{p.command}, p.RecorderFrame());
        CheckVk(VKF(EndCommandBuffer)(p.command), "XR capture end");

        const auto [wait, signal] = p.recorder.SubmitSemaphores();
        const VkSemaphore wait_semaphore = static_cast<VkSemaphore>(wait);
        const VkSemaphore signal_semaphore = static_cast<VkSemaphore>(signal);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = wait_semaphore ? 1 : 0;
        submit.pWaitSemaphores = &wait_semaphore;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &p.command;
        submit.signalSemaphoreCount = signal_semaphore ? 1 : 0;
        submit.pSignalSemaphores = &signal_semaphore;
        {
            std::scoped_lock lock(p.vk.QueueMutex());
            CheckVk(VKF(QueueSubmit)(p.vk.GetGraphicsQueue(), 1, &submit, p.fence),
                    "XR capture submit");
            if (p.encoding) p.recorder.Present();
        }
        CheckVk(VKF(WaitForFences)(device, 1, &p.fence, VK_TRUE, UINT64_MAX), "XR capture wait");
        CheckVk(VKF(ResetFences)(device, 1, &p.fence), "XR capture reset fence");

        if (p.shot) {
            vmaInvalidateAllocation(p.vk.GetAllocator(), p.readback_allocation, 0, VK_WHOLE_SIZE);
            const auto span = scrcpy::capture::SelectEye(full_width, 2, p.shot->eye);
            std::vector<uint8_t> rgba(size_t(span.width) * p.output_height * 4);
            const auto* source = static_cast<const uint8_t*>(p.readback_mapped);
            for (uint32_t y = 0; y < p.output_height; ++y)
                std::memcpy(rgba.data() + size_t(y) * span.width * 4,
                            source + (size_t(y) * full_width + span.x) * 4, size_t(span.width) * 4);
            // Opaque PNG: the composite's alpha carries no meaning.
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            std::thread([request = *p.shot, rgba = std::move(rgba), width = span.width,
                         height = p.output_height]() {
                std::string write_error;
                const std::filesystem::path path{request.path};
                const auto partial = std::filesystem::path{request.path + ".partial"};
                try {
                    std::filesystem::create_directories(path.parent_path());
                    if (!WritePng(partial, rgba, width, height))
                        throw std::runtime_error("PNG write failed");
                    std::filesystem::rename(partial, path);
                } catch (const std::exception& e) {
                    write_error = e.what();
                    std::error_code ignored;
                    std::filesystem::remove(partial, ignored);
                }
                EmbeddedScreenshots().Complete(request.token, width, height,
                                               std::move(write_error), 2);
            }).detach();
            p.shot.reset();
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (!error.empty()) {
        LOG_ERROR(Render_Vulkan, "XR capture compose failed: {}", error);
        if (p.shot) EmbeddedScreenshots().Complete(p.shot->token, 0, 0, error);
        p.shot.reset();
        p.failed = true;
    }
}
} // namespace Vulkan::OpenXr
