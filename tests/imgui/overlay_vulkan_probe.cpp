// SPDX-License-Identifier: GPL-2.0-or-later
// Real offscreen Vulkan rendering with the production backend; no Android/game claims.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <dlfcn.h>
#include <imgui.h>
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "spatial/imgui/overlay/OverlayComponents.hpp"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
void assert_fail_debug_msg(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}
static void check(vk::Result r) {
    if (r != vk::Result::eSuccess)
        assert_fail_debug_msg(vk::to_string(r).c_str());
}
template <class T>
static T take(vk::ResultValue<T> r) {
    check(r.result);
    return r.value;
}
int main(int argc, char** argv) {
    void* loader = dlopen(argc > 1 ? argv[1] : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")));
    std::vector<const char*> instance_ext;
    for (const auto& e : take(vk::enumerateInstanceExtensionProperties()))
        if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
            instance_ext.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    vk::ApplicationInfo app{.pApplicationName = "shadps4 SDF probe",
                            .apiVersion = VK_API_VERSION_1_2};
    auto instance = take(vk::createInstance(vk::InstanceCreateInfo{
        .flags = instance_ext.empty() ? vk::InstanceCreateFlags{}
                                      : vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
        .pApplicationInfo = &app,
        .enabledExtensionCount = uint32_t(instance_ext.size()),
        .ppEnabledExtensionNames = instance_ext.data()}));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
    auto physical = take(instance.enumeratePhysicalDevices()).front();
    const auto families = physical.getQueueFamilyProperties();
    uint32_t family = 0;
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eGraphics))
        ++family;
    float priority = 1.f;
    vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    std::vector<const char*> extensions{VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    for (const auto& e : take(physical.enumerateDeviceExtensionProperties()))
        if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0)
            extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    vk::PhysicalDeviceDynamicRenderingFeatures dynamic{.dynamicRendering = true};
    auto device = take(physical.createDevice(
        vk::DeviceCreateInfo{.pNext = &dynamic,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &queue_info,
                             .enabledExtensionCount = uint32_t(extensions.size()),
                             .ppEnabledExtensionNames = extensions.data()}));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    auto queue = device.getQueue(family, 0);
    const auto memory_types = physical.getMemoryProperties();
    auto allocate = [&](vk::MemoryRequirements req, vk::MemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory_types.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memory_types.memoryTypes[i].propertyFlags & flags) == flags)
                return take(
                    device.allocateMemory({.allocationSize = req.size, .memoryTypeIndex = i}));
        assert_fail_debug_msg("No memory type");
        return vk::DeviceMemory{};
    };
    constexpr uint32_t w = 1280, h = 720;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    auto target = take(device.createImage({.imageType = vk::ImageType::e2D,
                                           .format = format,
                                           .extent = {w, h, 1},
                                           .mipLevels = 1,
                                           .arrayLayers = 1,
                                           .samples = vk::SampleCountFlagBits::e1,
                                           .tiling = vk::ImageTiling::eOptimal,
                                           .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                    vk::ImageUsageFlagBits::eTransferSrc}));
    auto target_memory = allocate(device.getImageMemoryRequirements(target),
                                  vk::MemoryPropertyFlagBits::eDeviceLocal);
    check(device.bindImageMemory(target, target_memory, 0));
    vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};
    auto view = take(device.createImageView({.image = target,
                                             .viewType = vk::ImageViewType::e2D,
                                             .format = format,
                                             .subresourceRange = range}));
    auto readback = take(
        device.createBuffer({.size = w * h * 4, .usage = vk::BufferUsageFlagBits::eTransferDst}));
    auto readback_memory = allocate(device.getBufferMemoryRequirements(readback),
                                    vk::MemoryPropertyFlagBits::eHostVisible |
                                        vk::MemoryPropertyFlagBits::eHostCoherent);
    check(device.bindBufferMemory(readback, readback_memory, 0));
    auto pool = take(device.createCommandPool(
        {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer, .queueFamilyIndex = family}));
    auto command = take(device.allocateCommandBuffers({.commandPool = pool,
                                                       .level = vk::CommandBufferLevel::ePrimary,
                                                       .commandBufferCount = 1}))
                       .front();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {w, h};
    io.DeltaTime = 1.f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImFontConfig font_config;
    font_config.SizePixels = 26;
    font_config.RasterizerDensity = 2;
    io.FontDefault = io.Fonts->AddFontFromFileTTF("src/imgui/renderer/fonts/NotoSans-Regular.ttf",
                                                  26, &font_config);
    if (!io.FontDefault)
        return 5;
    io.Fonts->TexGlyphPadding = 7;
    io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
    std::mutex queue_mutex;
    ImGui::Vulkan::Init({.instance = instance,
                         .physical_device = physical,
                         .device = device,
                         .queue_family = family,
                         .queue = queue,
                         .queue_mutex = &queue_mutex,
                         .image_count = 2,
                         .min_allocation_size = 4096,
                         .pipeline_rendering_create_info = {.colorAttachmentCount = 1,
                                                            .pColorAttachmentFormats = &format},
                         .check_vk_result_fn = check});
    namespace ov = spatial::imgui::overlay;
    ov::OverlayShell shell;
    ov::OverlayInitInfo init;
    init.requested_font_mode = spatial::imgui::FontRenderMode::Sdf;
    init.renderer_capabilities = ImGui::Vulkan::FontCapabilities();
    shell.initialize(init);
    ov::PresentationEnvironment env;
    env.output.extent = {w, h};
    env.views.push_back(env.output);
    shell.setPresentation(env);
    ov::OverlaySnapshot snapshot;
    snapshot.status.presentation_fps = 59.8;
    snapshot.status.summary_items.push_back({ov::StableId("gpu"),
                                             ov::LocalizedText("GPU"),
                                             "12.4 ms",
                                             ov::StatusSeverity::Normal,
                                             {{ov::ThemeColor{.6f, .9f, .7f, 1}}, {}}});
    snapshot.status.summary_items.push_back({ov::StableId("uploads"),
                                             ov::LocalizedText("Re-uploads"),
                                             "4.2/frame",
                                             ov::StatusSeverity::Warning,
                                             {{ov::ThemeColor{1, .65f, .25f, 1}}, {}}});
    ov::OverlayTheme theme;
    ov::OverlayMetrics metrics;
    ov::ImeDialogState ime;
    ov::OverlayIntent summary;
    summary.kind = ov::OverlayIntentKind::SetStatusMode;
    summary.status_mode = ov::StatusMode::Summary;
    shell.controller().submitIntent(summary);
    const bool only_fps = argc > 3 && std::string_view(argv[3]) == "fps";
    if (only_fps) {
        metrics.simple_height_dp = 32;
        metrics.minimum_touch_target_dp = 32;
        summary.status_mode = ov::StatusMode::Simple;
        shell.controller().submitIntent(summary);
    }
    bool first = true;
    for (int frame_index = 0; frame_index < 10; ++frame_index) {
        auto fonts = shell.fonts();
        for (auto& font : fonts.roles)
            font.handle = io.FontDefault;
        shell.setResolvedFonts(fonts);
        shell.setSdfPipelineEvidence(ImGui::Vulkan::FontEvidence());
        // Match the host: measure and route touch before NewFrame.
        ov::measureOverlayStatus(snapshot, shell.fonts(), theme, metrics);
        shell.setMetrics(metrics);
        auto frame = shell.render(io.DeltaTime);
        ImGui::NewFrame();
        ov::applyOverlayTheme(theme, 1);
        ov::drawOverlayShellFrame({.frame = &frame, .snapshot = &snapshot, .theme = &theme},
                                  shell.controller().imeSession(), ime);
        if (!only_fps) {
            ImGui::SetNextWindowPos({680, 80});
            ImGui::SetNextWindowSize({550, 440});
            ImGui::Begin("SDF sizes", nullptr, ImGuiWindowFlags_NoSavedSettings);
            for (float size : {18.f, 26.f, 40.f, 64.f}) {
                ImGui::PushFont(io.FontDefault, size);
                ImGui::Text("SDF %.0f: Aa 0123", size);
                ImGui::PopFont();
            }
            if (frame_index >= 3)
                ImGui::Text("Late glyphs: @ & # %% + =");
            ImGui::End();
        }
        ImGui::Render();
        if (frame_index >= 7 && ImGui::GetDrawData()->Textures)
            for (const auto* texture : *ImGui::GetDrawData()->Textures)
                if (texture->Status == ImTextureStatus_WantCreate ||
                    texture->Status == ImTextureStatus_WantUpdates)
                    assert_fail_debug_msg("Static UI keeps requesting font uploads");
        ImGui::Vulkan::UpdateTextures(*ImGui::GetDrawData());
        check(command.reset());
        check(command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        vk::ImageMemoryBarrier barrier{
            .srcAccessMask = first ? vk::AccessFlags{} : vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = target,
            .subresourceRange = range};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
        vk::RenderingAttachmentInfo attachment{.imageView = view,
                                               .imageLayout =
                                                   vk::ImageLayout::eColorAttachmentOptimal,
                                               .loadOp = vk::AttachmentLoadOp::eClear,
                                               .storeOp = vk::AttachmentStoreOp::eStore};
        command.beginRenderingKHR({.renderArea = {{0, 0}, {w, h}},
                                   .layerCount = 1,
                                   .colorAttachmentCount = 1,
                                   .pColorAttachments = &attachment});
        ImGui::Vulkan::RenderDrawData(*ImGui::GetDrawData(), command);
        command.endRenderingKHR();
        barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
        command.copyImageToBuffer(
            target, vk::ImageLayout::eTransferSrcOptimal, readback,
            vk::BufferImageCopy{.imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                     .layerCount = 1},
                                .imageExtent = {w, h, 1}});
        check(command.end());
        check(queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &command}));
        check(queue.waitIdle());
        first = false;
    }
    const auto evidence = ImGui::Vulkan::FontEvidence();
    if (!evidence.atlas_generated || !evidence.sdf_texture_bound || !evidence.sdf_shader_bound)
        return 3;
    auto* pixels =
        static_cast<unsigned char*>(take(device.mapMemory(readback_memory, 0, w * h * 4)));
    std::ofstream ppm(argc > 2 ? argv[2] : "overlay-sdf.ppm", std::ios::binary);
    ppm << "P6\n" << w << " " << h << "\n255\n";
    size_t nonblack = 0;
    for (size_t i = 0; i < w * h; ++i) {
        ppm.write(reinterpret_cast<char*>(pixels + i * 4), 3);
        nonblack += pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2];
    }
    device.unmapMemory(readback_memory);
    if (nonblack < (only_fps ? 500 : 10000))
        return 4;
    std::printf("%s; pixels=%zu; GPU=%s\n", ImGui::Vulkan::FontDiagnostics().c_str(), nonblack,
                physical.getProperties().deviceName.data());
    ImGui::Vulkan::Shutdown();
    ImGui::DestroyContext();
    device.destroyCommandPool(pool);
    device.destroyBuffer(readback);
    device.freeMemory(readback_memory);
    device.destroyImageView(view);
    device.destroyImage(target);
    device.freeMemory(target_memory);
    device.destroy();
    instance.destroy();
    return 0;
}
