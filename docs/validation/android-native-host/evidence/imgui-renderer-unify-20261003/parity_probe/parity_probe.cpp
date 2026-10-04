// Parity probe: the same ImGui frames through the old shadPS4 Vulkan backend and the new Foundation
// renderer (dynamic rendering, and render-pass mode drawing the first renderer's textures).
// Every frame's three images must be byte-identical. Three frames stay in flight so premature
// destruction shows up in the validation layer.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <imgui.h>

#include "imgui_impl_vulkan.h"
#include "spatial/imgui/VulkanRenderer.hpp"
#include "upstream/imgui_impl_vulkan.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

void assert_fail_debug_msg(const char* message) {
    std::fprintf(stderr, "assert: %s\n", message);
    std::abort();
}

namespace {

const char* g_phase = "setup";
int g_errors = 0;
int g_warnings = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                       VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (++g_errors <= 20) {
            std::fprintf(stderr, "[%s] VALIDATION ERROR: %s\n", g_phase, data->pMessage);
        }
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (++g_warnings <= 10) {
            std::fprintf(stderr, "[%s] validation warning: %s\n", g_phase, data->pMessage);
        }
    }
    return VK_FALSE;
}

void Check(vk::Result result, const char* what) {
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr, "%s failed: %s\n", what, vk::to_string(result).c_str());
        std::exit(10);
    }
}

template <class T>
T Take(vk::ResultValue<T> result, const char* what) {
    Check(result.result, what);
    return std::move(result.value);
}

constexpr std::uint32_t Width = 1280;
constexpr std::uint32_t Height = 720;
constexpr int Slots = 3;
constexpr vk::Format Format = vk::Format::eR8G8B8A8Unorm;

struct Target {
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::Framebuffer framebuffer;
    vk::Buffer readback;
    vk::DeviceMemory readback_memory;
    const std::uint8_t* pixels = nullptr;
};

struct Slot {
    vk::CommandBuffer command;
    vk::Fence fence;
    std::array<Target, 3> targets;
    int frame = -1;
};

std::vector<std::uint8_t> Pattern(int seed) {
    std::vector<std::uint8_t> pixels(64 * 64 * 4);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            std::uint8_t* p = &pixels[(y * 64 + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 4 + seed * 40);
            p[1] = static_cast<std::uint8_t>(y * 4);
            p[2] = static_cast<std::uint8_t>((x ^ y) * 4 + seed * 90);
            p[3] = static_cast<std::uint8_t>(128 + (x + y) % 128);
        }
    }
    return pixels;
}

void SetupContext(ImGuiIO& io, bool alpha8) {
    io.IniFilename = nullptr;
    io.DisplaySize = {static_cast<float>(Width), static_cast<float>(Height)};
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    ImFontConfig config{};
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.RasterizerDensity = 2.0F;
    io.Fonts->TexGlyphPadding = 7;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight | ImFontAtlasFlags_NoBakedLines;
    if (alpha8) {
        io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;
    }
    io.FontDefault =
        io.Fonts->AddFontFromFileTTF("src/imgui/renderer/fonts/NotoSans-Regular.ttf", 32, &config);
    ImFontConfig merge = config;
    merge.MergeMode = true;
    io.Fonts->AddFontFromFileTTF("src/imgui/renderer/fonts/NotoSansCJK-Regular.ttc", 32, &merge);
    if (io.FontDefault == nullptr) {
        assert_fail_debug_msg("fonts not found: run from the repository root");
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
}

void DrawUi(int frame, ImTextureID image) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({20, 20});
    ImGui::SetNextWindowSize({760, 660});
    ImGui::Begin("Parity", nullptr, ImGuiWindowFlags_NoSavedSettings);
    for (float size : {14.0F, 20.0F, 32.0F, 48.0F, 72.0F}) {
        ImGui::PushFont(nullptr, size);
        ImGui::Text("%.0f: The quick brown fox 0123", size);
        ImGui::PopFont();
    }
    if (frame >= 3) {
        ImGui::Text("Late glyphs: @#$%%^&*()_+{}|:<>? ~");
    }
    if (frame >= 5) {
        ImGui::PushFont(nullptr, 28.0F);
        ImGui::Text("中文字形测试 汉字渲染 部分上传");
        ImGui::PopFont();
    }
    if (frame >= 8) {
        ImGui::PushFont(nullptr, 40.0F);
        ImGui::Text("龍鳳麒麟 雲霧繚繞 %d", frame);
        ImGui::PopFont();
    }
    if (image != ImTextureID_Invalid) {
        ImGui::Image(ImTextureRef(image), {128, 128});
        ImGui::SameLine();
        ImGui::Image(ImTextureRef(image), {64, 192}, {0.25F, 0.0F}, {1.5F, 2.0F});
    }
    static bool checked = true;
    ImGui::Checkbox("Checkbox", &checked);
    ImGui::ProgressBar(0.37F + frame * 0.01F, {300, 0});
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    list->AddRectFilled(origin, {origin.x + 120, origin.y + 60}, IM_COL32(200, 80, 40, 255), 8.0F);
    list->AddCircle({origin.x + 200, origin.y + 30}, 28, IM_COL32(40, 220, 120, 255), 0, 3.0F);
    list->AddLine({origin.x + 260, origin.y}, {origin.x + 380, origin.y + 60},
                  IM_COL32(255, 255, 0, 200), 4.0F);
    list->AddTriangleFilled({origin.x + 400, origin.y + 60}, {origin.x + 460, origin.y},
                            {origin.x + 520, origin.y + 60}, IM_COL32(90, 90, 255, 160));
    ImGui::Dummy({520, 64});
    ImGui::End();
    ImGui::Render();
}

std::uint32_t FindMemory(const vk::PhysicalDeviceMemoryProperties& properties, std::uint32_t bits,
                         vk::MemoryPropertyFlags flags) {
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    assert_fail_debug_msg("no memory type");
    return 0;
}

void WritePpm(const std::string& path, const std::uint8_t* pixels) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << Width << " " << Height << "\n255\n";
    for (std::size_t i = 0; i < std::size_t(Width) * Height; ++i) {
        out.write(reinterpret_cast<const char*>(pixels + i * 4), 3);
    }
}

int g_mismatches = 0;

void Compare(const Target& a, const Target& b, const char* label, int frame,
             const std::string& dump_dir) {
    const std::size_t bytes = std::size_t(Width) * Height * 4;
    if (std::memcmp(a.pixels, b.pixels, bytes) == 0) {
        return;
    }
    std::size_t pixels = 0;
    int max_delta = 0;
    for (std::size_t i = 0; i < bytes; i += 4) {
        bool differs = false;
        for (int c = 0; c < 4; ++c) {
            const int delta = std::abs(int(a.pixels[i + c]) - int(b.pixels[i + c]));
            max_delta = std::max(max_delta, delta);
            differs |= delta != 0;
        }
        pixels += differs ? 1 : 0;
    }
    std::fprintf(stderr, "frame %d %s: %zu pixels differ, max channel delta %d\n", frame, label,
                 pixels, max_delta);
    if (++g_mismatches <= 3) {
        WritePpm(dump_dir + "/mismatch-" + std::to_string(frame) + "-" + label + "-a.ppm",
                 a.pixels);
        WritePpm(dump_dir + "/mismatch-" + std::to_string(frame) + "-" + label + "-b.ppm",
                 b.pixels);
    }
}

void RecordShiftedTwice(ImDrawData* data, const auto& render) {
    render();
    const ImVec2 saved = data->DisplayPos;
    data->DisplayPos = ImVec2(-700.0F, -380.0F);
    render();
    data->DisplayPos = saved;
}

} // namespace

int main(int argc, char** argv) {
    bool alpha8 = false;
    bool upstream = false;
    int device_index = 0;
    int frames = 16;
    std::string dump_dir = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--alpha8") {
            alpha8 = true;
        } else if (arg == "--device" && i + 1 < argc) {
            device_index = std::atoi(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (arg == "--upstream") {
            upstream = true;
        } else if (arg == "--dump" && i + 1 < argc) {
            dump_dir = argv[++i];
        }
    }
#ifdef _WIN32
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    auto get_instance_proc_addr = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                                GetProcAddress(library, "vkGetInstanceProcAddr"))
                                          : nullptr;
#else
    void* library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    auto get_instance_proc_addr = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                                dlsym(library, "vkGetInstanceProcAddr"))
                                          : nullptr;
#endif
    if (get_instance_proc_addr == nullptr) {
        std::fprintf(stderr, "Vulkan loader not found\n");
        return 2;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

    std::vector<const char*> layers;
    std::vector<const char*> instance_extensions;
    for (const auto& layer : Take(vk::enumerateInstanceLayerProperties(), "layers")) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
        }
    }
    bool debug_utils = false;
    for (const auto& ext : Take(vk::enumerateInstanceExtensionProperties(), "extensions")) {
        if (std::strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
            debug_utils = true;
            instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        if (std::strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
    }
    const vk::ApplicationInfo app{.pApplicationName = "imgui renderer parity",
                                  .apiVersion = VK_API_VERSION_1_2};
    const vk::Instance instance = Take(
        vk::createInstance(vk::InstanceCreateInfo{
            .pApplicationInfo = &app,
            .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size()),
            .ppEnabledExtensionNames = instance_extensions.data()}),
        "vkCreateInstance");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (debug_utils) {
        const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = &OnDebug,
        };
        Check(static_cast<vk::Result>(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDebugUtilsMessengerEXT(
                  instance, &messenger_info, nullptr, &messenger)),
              "messenger");
    }
    const auto physical_devices = Take(instance.enumeratePhysicalDevices(), "devices");
    const vk::PhysicalDevice physical = physical_devices.at(device_index);
    const auto properties = physical.getProperties();
    const auto memory_properties = physical.getMemoryProperties();
    const auto families = physical.getQueueFamilyProperties();
    std::uint32_t family = 0;
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eGraphics)) {
        ++family;
    }
    const float priority = 1.0F;
    const vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    std::vector<const char*> device_extensions{VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    const vk::PhysicalDeviceDynamicRenderingFeatures dynamic_rendering{.dynamicRendering = true};
    const vk::Device device = Take(
        physical.createDevice(vk::DeviceCreateInfo{
            .pNext = &dynamic_rendering,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_info,
            .enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size()),
            .ppEnabledExtensionNames = device_extensions.data()}),
        "vkCreateDevice");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    const vk::Queue queue = device.getQueue(family, 0);
    std::printf("GPU: %s, validation %s\n", properties.deviceName.data(),
                layers.empty() ? "off" : "on");

    const vk::AttachmentDescription attachment{
        .format = Format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal};
    const vk::AttachmentReference color_reference{
        .attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal};
    const vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &color_reference};
    const vk::RenderPass render_pass = Take(
        device.createRenderPass(vk::RenderPassCreateInfo{
            .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass}),
        "render pass");

    const vk::CommandPool pool = Take(
        device.createCommandPool({.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                  .queueFamilyIndex = family}),
        "command pool");
    std::array<Slot, Slots> slots;
    for (Slot& slot : slots) {
        slot.command = Take(device.allocateCommandBuffers({.commandPool = pool,
                                                           .level = vk::CommandBufferLevel::ePrimary,
                                                           .commandBufferCount = 1}),
                            "command buffer")
                           .front();
        slot.fence = Take(device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}), "fence");
        for (Target& target : slot.targets) {
            target.image = Take(
                device.createImage({.imageType = vk::ImageType::e2D,
                                    .format = Format,
                                    .extent = {Width, Height, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = vk::SampleCountFlagBits::e1,
                                    .tiling = vk::ImageTiling::eOptimal,
                                    .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                             vk::ImageUsageFlagBits::eTransferSrc}),
                "image");
            const auto requirements = device.getImageMemoryRequirements(target.image);
            target.memory = Take(device.allocateMemory(
                                     {.allocationSize = requirements.size,
                                      .memoryTypeIndex = FindMemory(memory_properties,
                                                                    requirements.memoryTypeBits,
                                                                    vk::MemoryPropertyFlagBits::eDeviceLocal)}),
                                 "image memory");
            Check(device.bindImageMemory(target.image, target.memory, 0), "bind image");
            target.view = Take(device.createImageView(
                                   {.image = target.image,
                                    .viewType = vk::ImageViewType::e2D,
                                    .format = Format,
                                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}),
                               "view");
            target.framebuffer = Take(device.createFramebuffer({.renderPass = render_pass,
                                                                .attachmentCount = 1,
                                                                .pAttachments = &target.view,
                                                                .width = Width,
                                                                .height = Height,
                                                                .layers = 1}),
                                      "framebuffer");
            target.readback = Take(device.createBuffer({.size = Width * Height * 4,
                                                        .usage = vk::BufferUsageFlagBits::eTransferDst}),
                                   "readback");
            const auto buffer_requirements = device.getBufferMemoryRequirements(target.readback);
            target.readback_memory = Take(
                device.allocateMemory(
                    {.allocationSize = buffer_requirements.size,
                     .memoryTypeIndex = FindMemory(memory_properties, buffer_requirements.memoryTypeBits,
                                                   vk::MemoryPropertyFlagBits::eHostVisible |
                                                       vk::MemoryPropertyFlagBits::eHostCoherent)}),
                "readback memory");
            Check(device.bindBufferMemory(target.readback, target.readback_memory, 0), "bind buffer");
            target.pixels = static_cast<const std::uint8_t*>(
                Take(device.mapMemory(target.readback_memory, 0, VK_WHOLE_SIZE), "map"));
        }
    }

    // Context A: the reference backend. shadPS4's old backend draws SDF text; the stock upstream
    // backend (what the XR layers used) draws the bitmap atlas with a clamping sampler.
    ImGuiContext* context_old = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_old);
    SetupContext(ImGui::GetIO(), alpha8);
    std::mutex queue_mutex;
    vk::Format old_format = Format;
    VkFormat upstream_format = static_cast<VkFormat>(Format);
    auto pattern = Pattern(1);
    ImGui::Vulkan::UploadTextureData old_image{};
    struct LoaderState {
        PFN_vkGetInstanceProcAddr get_instance_proc_addr;
        VkInstance instance;
        VkDevice device;
    } loader_state{get_instance_proc_addr, instance, device};
    if (upstream) {
        ImGui_ImplVulkan_LoadFunctions(
            VK_API_VERSION_1_2,
            [](const char* name, void* user_data) -> PFN_vkVoidFunction {
                const auto* state = static_cast<const LoaderState*>(user_data);
                PFN_vkVoidFunction function = state->get_instance_proc_addr(state->instance, name);
                // Surface entry points belong to an extension this probe does not enable; the stock
                // backend only uses them for its window helpers.
                if (function == nullptr && std::strstr(name, "Surface") == nullptr) {
                    const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                        state->get_instance_proc_addr(state->instance, "vkGetDeviceProcAddr"));
                    function = get_device_proc_addr(state->device, name);
                }
                if (function == nullptr) {
                    function = reinterpret_cast<PFN_vkVoidFunction>(+[] {
                        assert_fail_debug_msg("unavailable Vulkan entry point called");
                    });
                }
                return function;
            },
            &loader_state);
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_2;
        init.Instance = instance;
        init.PhysicalDevice = physical;
        init.Device = device;
        init.QueueFamily = family;
        init.Queue = queue;
        init.MinImageCount = 2;
        init.ImageCount = 8;
        init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.DescriptorPoolSize = 64;
        init.UseDynamicRendering = true;
        init.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &upstream_format};
        init.CheckVkResultFn = [](VkResult r) { Check(static_cast<vk::Result>(r), "upstream"); };
        if (!ImGui_ImplVulkan_Init(&init)) {
            return 4;
        }
    } else {
        ImGui::Vulkan::Init({.instance = instance,
                             .physical_device = physical,
                             .device = device,
                             .queue_family = family,
                             .queue = queue,
                             .queue_mutex = &queue_mutex,
                             .image_count = 8,
                             .min_allocation_size = 4096,
                             .pipeline_rendering_create_info = {.colorAttachmentCount = 1,
                                                                .pColorAttachmentFormats = &old_format},
                             .check_vk_result_fn = [](vk::Result r) { Check(r, "old backend"); }});
        old_image = ImGui::Vulkan::UploadTexture(pattern.data(), Format, 64, 64, pattern.size());
        old_image.Upload();
    }

    // Context N: the new renderer, twice.
    // CreateContext keeps the previous context current.
    ImGuiContext* context_new = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_new);
    SetupContext(ImGui::GetIO(), alpha8);
    const auto on_error = [](VkResult result, const char* operation, void*) {
        std::fprintf(stderr, "renderer error %d in %s\n", static_cast<int>(result), operation);
        std::exit(11);
    };
    spatial::imgui::VulkanRendererCreateInfo create_info{
        .instance = instance,
        .physical_device = physical,
        .device = device,
        .get_instance_proc_addr = get_instance_proc_addr,
        .color_format = static_cast<VkFormat>(Format),
        .sdf_fonts = !upstream,
        .sampler_address_mode =
            upstream ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .on_error = on_error,
    };
    std::string error;
    auto dynamic_renderer = spatial::imgui::VulkanRenderer::Create(create_info, &error);
    create_info.render_pass = render_pass;
    create_info.color_format = VK_FORMAT_UNDEFINED;
    auto pass_renderer = spatial::imgui::VulkanRenderer::Create(create_info, &error);
    if (!dynamic_renderer || !pass_renderer) {
        std::fprintf(stderr, "renderer creation failed: %s\n", error.c_str());
        return 3;
    }
    spatial::imgui::VulkanTexture* new_image =
        upstream ? nullptr : dynamic_renderer->CreateTexture(pattern.data(), 64, 64);

    std::uint64_t completed = 0;
    const auto compare_slot = [&](Slot& slot) {
        if (slot.frame < 0) {
            return;
        }
        Check(device.waitForFences(slot.fence, true, UINT64_MAX), "wait");
        Compare(slot.targets[0], slot.targets[1], "old-vs-dynamic", slot.frame, dump_dir);
        Compare(slot.targets[1], slot.targets[2], "dynamic-vs-renderpass", slot.frame, dump_dir);
        completed = std::max<std::uint64_t>(completed, std::uint64_t(slot.frame) + 1);
        slot.frame = -1;
    };

    for (int frame = 0; frame < frames; ++frame) {
        Slot& slot = slots[frame % Slots];
        compare_slot(slot);
        Check(device.resetFences(slot.fence), "reset fence");

        if (frame == 9 && !upstream) {
            // Replace the image texture while earlier frames that sample it are in flight.
            pattern = Pattern(2);
            g_phase = "old";
            old_image.Destroy();
            old_image = ImGui::Vulkan::UploadTexture(pattern.data(), Format, 64, 64, pattern.size());
            old_image.Upload();
            g_phase = "new";
            if (new_image != nullptr) {
        dynamic_renderer->ReleaseTexture(new_image);
    }
            new_image = dynamic_renderer->CreateTexture(pattern.data(), 64, 64);
        }

        ImGui::SetCurrentContext(context_old);
        if (upstream) {
            ImGui_ImplVulkan_NewFrame();
        }
        DrawUi(frame, upstream ? ImTextureID_Invalid : old_image.im_texture);
        ImDrawData* old_data = ImGui::GetDrawData();
        ImGui::SetCurrentContext(context_new);
        DrawUi(frame, upstream ? ImTextureID_Invalid
                               : spatial::imgui::ToTextureId<ImTextureID>(new_image));
        ImDrawData* new_data = ImGui::GetDrawData();

        g_phase = "old";
        ImGui::SetCurrentContext(context_old);
        if (upstream) {
            for (ImTextureData* texture : *old_data->Textures) {
                if (texture->Status != ImTextureStatus_OK) {
                    ImGui_ImplVulkan_UpdateTexture(texture);
                }
            }
        } else {
            ImGui::Vulkan::UpdateTextures(*old_data);
        }

        const vk::CommandBuffer command = slot.command;
        Check(command.reset(), "reset");
        Check(command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}), "begin");
        g_phase = "new";
        const std::uint64_t serial = std::uint64_t(frame) + 1;
        dynamic_renderer->BeginFrame(serial, completed);
        dynamic_renderer->UpdateTextures(command, new_data);
        pass_renderer->BeginFrame(serial, completed);
        pass_renderer->UpdateTextures(command, new_data);

        std::array<vk::ImageMemoryBarrier, 3> to_color{};
        for (int i = 0; i < 3; ++i) {
            to_color[i] = vk::ImageMemoryBarrier{
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = slot.targets[i].image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        }
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                                to_color);

        VkClearValue raw_clear{};
        raw_clear.color.float32[0] = 0.05F;
        raw_clear.color.float32[1] = 0.05F;
        raw_clear.color.float32[2] = 0.08F;
        raw_clear.color.float32[3] = 1.0F;
        const vk::ClearValue& clear = reinterpret_cast<const vk::ClearValue&>(raw_clear);
        const auto begin_rendering = [&](const Target& target) {
            const vk::RenderingAttachmentInfo color{.imageView = target.view,
                                                    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                    .loadOp = vk::AttachmentLoadOp::eClear,
                                                    .storeOp = vk::AttachmentStoreOp::eStore,
                                                    .clearValue = clear};
            command.beginRenderingKHR({.renderArea = {{0, 0}, {Width, Height}},
                                       .layerCount = 1,
                                       .colorAttachmentCount = 1,
                                       .pColorAttachments = &color});
        };
        g_phase = "old";
        begin_rendering(slot.targets[0]);
        RecordShiftedTwice(old_data, [&] {
            if (upstream) {
                ImGui_ImplVulkan_RenderDrawData(old_data, command);
            } else {
                ImGui::Vulkan::RenderDrawData(*old_data, command);
            }
        });
        command.endRenderingKHR();
        g_phase = "new";
        begin_rendering(slot.targets[1]);
        RecordShiftedTwice(new_data, [&] { dynamic_renderer->RenderDrawData(command, *new_data); });
        command.endRenderingKHR();
        command.beginRenderPass({.renderPass = render_pass,
                                 .framebuffer = slot.targets[2].framebuffer,
                                 .renderArea = {{0, 0}, {Width, Height}},
                                 .clearValueCount = 1,
                                 .pClearValues = &clear},
                                vk::SubpassContents::eInline);
        RecordShiftedTwice(new_data, [&] { pass_renderer->RenderDrawData(command, *new_data); });
        command.endRenderPass();

        std::array<vk::ImageMemoryBarrier, 3> to_transfer{};
        for (int i = 0; i < 3; ++i) {
            to_transfer[i] = vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = slot.targets[i].image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        }
        command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_transfer);
        for (const Target& target : slot.targets) {
            command.copyImageToBuffer(
                target.image, vk::ImageLayout::eTransferSrcOptimal, target.readback,
                vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                    .imageExtent = {Width, Height, 1}});
        }
        const vk::MemoryBarrier to_host{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                        .dstAccessMask = vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, to_host, {}, {});
        Check(command.end(), "end");
        {
            std::scoped_lock lock{queue_mutex};
            Check(queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &command},
                               slot.fence),
                  "submit");
        }
        slot.frame = frame;
    }
    for (Slot& slot : slots) {
        compare_slot(slot);
    }
    Check(device.waitIdle(), "wait idle");

    const Slot& last = slots[(frames - 1) % Slots];
    WritePpm(dump_dir + "/parity-new.ppm", last.targets[1].pixels);
    std::size_t nonblack = 0;
    for (std::size_t i = 0; i < std::size_t(Width) * Height; ++i) {
        const std::uint8_t* p = last.targets[1].pixels + i * 4;
        nonblack += (p[0] > 20 || p[1] > 20 || p[2] > 30) ? 1 : 0;
    }
    const auto stats = dynamic_renderer->Stats();
    std::printf("dynamic: %s\n", dynamic_renderer->FontDiagnostics().c_str());
    std::printf("  full %llu partial %llu bytes %llu draws %llu sdf %llu skipped %llu live %u\n",
                (unsigned long long)stats.full_uploads, (unsigned long long)stats.partial_uploads,
                (unsigned long long)stats.upload_bytes, (unsigned long long)stats.draws,
                (unsigned long long)stats.sdf_draws, (unsigned long long)stats.skipped_draws,
                stats.live_textures);
    const auto pass_stats = pass_renderer->Stats();
    std::printf("render pass: draws %llu sdf %llu skipped %llu uploads %llu\n",
                (unsigned long long)pass_stats.draws, (unsigned long long)pass_stats.sdf_draws,
                (unsigned long long)pass_stats.skipped_draws,
                (unsigned long long)(pass_stats.full_uploads + pass_stats.partial_uploads));
    std::printf("lit pixels %zu, mismatching frames %d, validation errors %d warnings %d\n",
                nonblack, g_mismatches, g_errors, g_warnings);

    // Teardown in the documented order: detach ImGui textures while their context lives.
    g_phase = "teardown";
    ImGui::SetCurrentContext(context_new);
    dynamic_renderer->DetachImGuiTextures(
        {ImGui::GetPlatformIO().Textures.Data,
         static_cast<std::size_t>(ImGui::GetPlatformIO().Textures.Size)});
    pass_renderer->DetachImGuiTextures(
        {ImGui::GetPlatformIO().Textures.Data,
         static_cast<std::size_t>(ImGui::GetPlatformIO().Textures.Size)});
    if (new_image != nullptr) {
        dynamic_renderer->ReleaseTexture(new_image);
    }
    pass_renderer.reset();
    dynamic_renderer.reset();
    ImGui::DestroyContext(context_new);
    ImGui::SetCurrentContext(context_old);
    if (upstream) {
        ImGui_ImplVulkan_Shutdown();
    } else {
        old_image.Destroy();
        ImGui::Vulkan::Shutdown();
    }
    ImGui::DestroyContext(context_old);
    for (Slot& slot : slots) {
        device.destroyFence(slot.fence);
        for (Target& target : slot.targets) {
            device.unmapMemory(target.readback_memory);
            device.destroyBuffer(target.readback);
            device.freeMemory(target.readback_memory);
            device.destroyFramebuffer(target.framebuffer);
            device.destroyImageView(target.view);
            device.destroyImage(target.image);
            device.freeMemory(target.memory);
        }
    }
    device.destroyCommandPool(pool);
    device.destroyRenderPass(render_pass);
    device.destroy();
    if (messenger != VK_NULL_HANDLE) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    }
    instance.destroy();
    std::printf("teardown validation errors %d\n", g_errors);
    const bool ok = g_mismatches == 0 && g_errors == 0 && nonblack > 20000 &&
                    stats.partial_uploads > 0 && stats.skipped_draws == 0 &&
                    (stats.sdf_draws > 0) == !upstream;
    std::printf("%s\n", ok ? "PARITY OK" : "PARITY FAILED");
    return ok ? 0 : 1;
}
