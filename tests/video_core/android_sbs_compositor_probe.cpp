// SPDX-License-Identifier: GPL-2.0-or-later
// Real production post-process shader, array slice views and GPU readback.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

struct ProbeWindow : Frontend::Window {
    s32 GetWidth() const override { return 16; }
    s32 GetHeight() const override { return 8; }
    Frontend::WindowSystemInfo GetWindowInfo() const override { return {}; }
    bool RequestKeyboard() override { return false; }
    void ReleaseKeyboard() override {}
};

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("sbs-compositor-probe");
    ProbeWindow window;
    auto driver = std::string_view(argv[1]) == "system" ? Vulkan::LoadAndroidSystemDriver()
        : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    Vulkan::Scheduler scheduler(instance);
    const auto device = instance.GetDevice();
    constexpr u32 W = 16, H = 8, Layers = 6, PixelBytes = 4, SliceBytes = W * H * PixelBytes;
    const auto memory_type = [&](u32 bits, vk::MemoryPropertyFlags flags) {
        const auto props = instance.GetMemoryProperties();
        for (u32 i = 0; i < props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    };
    struct Buffer { vk::UniqueDeviceMemory memory; vk::UniqueBuffer buffer; void* mapped; };
    const auto make_buffer = [&](u32 bytes) {
        Buffer b;
        b.buffer = Vulkan::Check(device.createBufferUnique({.size = bytes,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst}));
        const auto req = device.getBufferMemoryRequirements(*b.buffer);
        b.memory = Vulkan::Check(device.allocateMemoryUnique({.allocationSize = req.size,
            .memoryTypeIndex = memory_type(req.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)}));
        Vulkan::Check(device.bindBufferMemory(*b.buffer, *b.memory, 0));
        b.mapped = Vulkan::Check(device.mapMemory(*b.memory, 0, VK_WHOLE_SIZE));
        return b;
    };
    struct Image { vk::UniqueDeviceMemory memory; vk::UniqueImage image; };
    const auto make_image = [&](u32 layers) {
        Image image;
        image.image = Vulkan::Check(device.createImageUnique({.imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm, .extent = {W, H, 1}, .mipLevels = 1,
            .arrayLayers = layers, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal, .usage = vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                vk::ImageUsageFlagBits::eColorAttachment}));
        const auto req = device.getImageMemoryRequirements(*image.image);
        image.memory = Vulkan::Check(device.allocateMemoryUnique({.allocationSize = req.size,
            .memoryTypeIndex = memory_type(req.memoryTypeBits, {})}));
        Vulkan::Check(device.bindImageMemory(*image.image, *image.memory, 0));
        return image;
    };
    auto input = make_image(Layers), output = make_image(1);
    auto upload = make_buffer(SliceBytes * Layers), readback = make_buffer(SliceBytes);
    std::array<vk::UniqueImageView, Layers> views;
    for (u32 layer = 0; layer < Layers; ++layer)
        views[layer] = Vulkan::Check(device.createImageViewUnique({.image = *input.image,
            .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, layer, 1}}));
    auto output_view = Vulkan::Check(device.createImageViewUnique({.image = *output.image,
        .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
    // Layer 0/1: distinct red/green eyes. Layer 2: shared red/green atlas.
    // Layer 3/4: distinct premultiplied blue overlays; layer 5: their atlas.
    auto* bytes = static_cast<u8*>(upload.mapped);
    for (u32 l = 0; l < Layers; ++l) for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x) {
        const bool right = l == 1 || l == 4 || ((l == 2 || l == 5) && x >= W / 2);
        const u8 red = y < H / 2 ? 255 : 128;
        const std::array<u8, 4> rgba = l < 3
            ? std::array<u8, 4>{u8(right ? 0 : red), u8(right ? red : 0), 0, u8(x % 2 ? 128 : 0)}
            : std::array<u8, 4>{0, 0, u8(right ? 128 : 64), u8(right ? 128 : 64)};
        std::memcpy(bytes + l * SliceBytes + (y * W + x) * PixelBytes, rgba.data(), PixelBytes);
    }
    const auto barrier = [&](vk::Image image, u32 layers, vk::ImageLayout old_layout,
                             vk::ImageLayout new_layout) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = old_layout, .newLayout = new_layout, .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers}};
        scheduler.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    barrier(*input.image, Layers, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
    scheduler.CommandBuffer().copyBufferToImage(*upload.buffer, *input.image,
        vk::ImageLayout::eTransferDstOptimal, vk::BufferImageCopy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, Layers}, .imageExtent = {W, H, 1}});
    barrier(*input.image, Layers, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
    Vulkan::HostPasses::PostProcessingPass pp;
    pp.Create(instance, vk::Format::eR8G8B8A8Unorm);
    Vulkan::Frame frame{};
    frame.width = W; frame.height = H; frame.image = *output.image; frame.image_view = *output_view;
    unsigned checks{}, failures{};
    for (const u32 mode : {0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u, 10u, 11u, 14u, 15u}) for (u32 flip = 0; flip < 2; ++flip) {
        barrier(*output.image, 1, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);
        Vulkan::HostPasses::PostProcessingPass::Settings settings;
        settings.hdr = 1; settings.sbs = mode; settings.flip_y = flip;
        const bool split = mode & 1, overlay = mode & 2, split_overlay = mode & 4;
        pp.Render(scheduler, *views[split ? 0 : 2], {W, H}, frame, settings,
            {*views[1], *views[split_overlay ? 3 : 5], *views[4]});
        barrier(*output.image, 1, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferSrcOptimal);
        scheduler.CommandBuffer().copyImageToBuffer(*output.image, vk::ImageLayout::eTransferSrcOptimal,
            *readback.buffer, vk::BufferImageCopy{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {W, H, 1}});
        scheduler.Finish(); // Test readback only; production adds no blocking wait.
        const auto* actual = static_cast<const u8*>(readback.mapped);
        for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x) {
            const bool right = x >= W / 2;
            const u32 source_y = flip ? H - 1 - y : y;
            const int intensity = source_y < H / 2 ? 255 : 128;
            const int alpha = overlay ? (right ? 128 : 64) : 0;
            const int base = std::lround(intensity * (255 - alpha) / 255.0);
            // Split-eye linear sampling straddles adjacent 0/128 texels.
            const int source_alpha = split ? 64 : (x % 2 ? 128 : 0);
            const std::array<int, 4> expected{right ? 0 : base, right ? base : 0, alpha,
                                             overlay || (mode & 8) ? 255 : source_alpha};
            for (u32 c = 0; c < 4; ++c) {
                ++checks;
                if (std::abs(int(actual[(y * W + x) * 4 + c]) - expected[c]) > 1) {
                    if (++failures < 8) std::printf("FAIL mode=%u flip=%u xy=%u,%u c=%u got=%u expected=%d\n",
                        mode, flip, x, y, c, actual[(y * W + x) * 4 + c], expected[c]);
                }
            }
        }
    }
    // Explicit rectangles must override shared-view heuristics. The base
    // samples only the bottom half of a dynamic atlas. Overlay rectangles swap
    // atlas eyes independently; border mode verifies the guest sampler reaches
    // the actual descriptor instead of silently using the PP edge sampler.
    auto border_sampler = Vulkan::Check(device.createSamplerUnique({
        .magFilter=vk::Filter::eLinear, .minFilter=vk::Filter::eLinear,
        .mipmapMode=vk::SamplerMipmapMode::eNearest,
        .addressModeU=vk::SamplerAddressMode::eClampToBorder,
        .addressModeV=vk::SamplerAddressMode::eClampToBorder,
        .addressModeW=vk::SamplerAddressMode::eClampToBorder,
        .borderColor=vk::BorderColor::eFloatTransparentBlack}));
    for (u32 variant=0;variant<5;++variant) for (u32 flip=0;flip<2;++flip) {
        barrier(*output.image,1,vk::ImageLayout::eUndefined,vk::ImageLayout::eColorAttachmentOptimal);
        Vulkan::HostPasses::PostProcessingPass::Settings settings;
        settings.hdr=1; settings.sbs=24u|((variant==1 || variant==2)?2u:0u); settings.flip_y=flip;
        settings.eye_uv={{{.25f,.25f,.125f,.625f},{.25f,.25f,.625f,.625f},
                          {.25f,1.f,variant==2?2.f:.625f,0.f},{.25f,1.f,variant==2?2.f:.125f,0.f}}};
        // UE and Unity supply opposite Y scales. Verify asymmetric rows so a
        // second Y flip cannot hide behind a uniform source rectangle.
        if (variant>=3) for (u32 eye=0;eye<2;++eye) {
            settings.eye_uv[eye][1]=variant==4?-1.f:1.f;
            settings.eye_uv[eye][3]=variant==4?1.f:0.f;
        }
        pp.Render(scheduler,*views[2],{W,H},frame,settings,
                  {*views[2],*views[5],*views[5]}, {},
                  std::array<vk::Sampler,4>{vk::Sampler{},vk::Sampler{},*border_sampler,*border_sampler});
        barrier(*output.image,1,vk::ImageLayout::eColorAttachmentOptimal,vk::ImageLayout::eTransferSrcOptimal);
        scheduler.CommandBuffer().copyImageToBuffer(*output.image,vk::ImageLayout::eTransferSrcOptimal,
            *readback.buffer,vk::BufferImageCopy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={W,H,1}});
        scheduler.Finish();
        const auto* actual=static_cast<const u8*>(readback.mapped);
        for(u32 y=0;y<H;++y) for(u32 x=0;x<W;++x) {
            const bool right=x>=W/2;
            const int alpha=variant==1?(right?64:128):0;
            const bool reverse=bool(flip) != (variant==4);
            const u32 source_y=reverse?H-1-y:y;
            const int intensity=variant>=3 && source_y<H/2?255:128;
            const int base=std::lround(intensity*(255-alpha)/255.0);
            const std::array<int,4> expected{right?0:base,right?base:0,alpha,255};
            for(u32 c=0;c<4;++c) {
                ++checks;
                if(std::abs(int(actual[(y*W+x)*4+c])-expected[c])>1) {
                    if(++failures<8)std::printf("FAIL explicit variant=%u flip=%u xy=%u,%u c=%u got=%u expected=%d\n",variant,flip,x,y,c,actual[(y*W+x)*4+c],expected[c]);
                }
            }
        }
    }
    device.unmapMemory(*upload.memory); device.unmapMemory(*readback.memory);
    std::printf("sbs_compositor_probe: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
