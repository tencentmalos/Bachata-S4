// SPDX-License-Identifier: GPL-2.0-or-later
#include "cinema_environment.h"
#include "status_projection.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <android/asset_manager.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <fmt/format.h>
#include "spatial/engine/Engine.h"
#include "spatial/engine/ViewRenderer.h"
#include "spatial/engine/VoxelGi.h"
#include "spatial/platform/android/JniHelper.h"
#include "spatial/scene/BasicScene.h"
#include "spatial/world/World.h"
#include "spatial/world/WorldAsset.h"
#include "spatial/xr/XrMath.h"
#include "spatial/xr/XrSceneVulkanLayer.h"

namespace Vulkan::OpenXr {
namespace {
namespace fs = std::filesystem;
namespace xm = spatial::xr::math;
using Clock = std::chrono::steady_clock;
std::mutex control_mutex;
std::optional<std::string> world_override;
std::string diagnostics = "cinema environment not created";

// Objects the runtime replaces: the game picture is the quad layer, the live
// PSV status display is StatusScene's own model on the World's dock.
constexpr const char* kRuntimeOwned[] = {"Game screen - preview", "PSV status display",
                                         "Viewer - seated"};

// The installed package's identity: this library's file changes with every
// APK install, so a different stamp re-extracts everything.
std::string InstallStamp() {
    Dl_info info{};
    struct stat st{};
    if (!dladdr(reinterpret_cast<void*>(&InstallStamp), &info) || !info.dli_fname ||
        stat(info.dli_fname, &st) != 0)
        return {};
    return fmt::format("{} {} {}", info.dli_fname, st.st_size, st.st_mtime);
}

// Copies xr/cinema (World documents, models, local covers) out of the APK
// once per install: the engine reads assets through a directory source.
void ExtractAssets(const fs::path& out, size_t& files, size_t& bytes) {
    auto* manager = spatial::platform::JniHelper::getAssetManager();
    if (!manager) throw std::runtime_error("cinema: no Android asset manager");
    const std::string stamp = InstallStamp();
    const fs::path stamp_file = out / ".install-stamp";
    std::string previous;
    if (std::ifstream in{stamp_file}) std::getline(in, previous);
    const bool fresh = stamp.empty() || previous != stamp;
    for (const char* dir : {"xr/cinema", "xr/cinema/models", "xr/cinema/models/local"}) {
        std::unique_ptr<AAssetDir, decltype(&AAssetDir_close)> listing(
            AAssetManager_openDir(manager, dir), AAssetDir_close);
        if (!listing) continue;
        const fs::path target = out / fs::path(dir).lexically_relative("xr/cinema");
        fs::create_directories(target);
        while (const char* name = AAssetDir_getNextFileName(listing.get())) {
            const std::string path = std::string(dir) + "/" + name;
            std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
                AAssetManager_open(manager, path.c_str(), AASSET_MODE_STREAMING), AAsset_close);
            if (!asset) continue;
            const auto size = AAsset_getLength64(asset.get());
            const fs::path file = target / name;
            std::error_code ec;
            if (!fresh && fs::exists(file, ec) && fs::file_size(file, ec) == uint64_t(size)) {
                ++files; bytes += size; continue;
            }
            const fs::path temp = file.string() + ".part";
            {
                std::ofstream o(temp, std::ios::binary | std::ios::trunc);
                std::vector<char> chunk(1 << 20);
                int64_t left = size;
                while (left > 0) {
                    const int n = AAsset_read(asset.get(), chunk.data(), chunk.size());
                    if (n <= 0) throw std::runtime_error("cinema: short asset read " + path);
                    o.write(chunk.data(), n);
                    left -= n;
                }
                if (!o) throw std::runtime_error("cinema: cannot write " + temp.string());
            }
            fs::rename(temp, file);
            ++files; bytes += size;
        }
    }
    if (fresh && !stamp.empty()) std::ofstream{stamp_file, std::ios::trunc} << stamp << '\n';
}

std::string Property(const char* name, const char* fallback) {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get(name, value) > 0 ? std::string(value) : std::string(fallback);
}
} // namespace

std::string CinemaWorld() {
    std::scoped_lock lock(control_mutex);
    if (world_override) return *world_override;
    return Property("debug.shadps4.xr_cinema_world", "tv-lounge");
}

XrPosef CinemaAuthoredOrigin(const XrPosef& screen_pose) {
    return StatusProjection::ToWorld(screen_pose,
        {xm::IdentityPose().orientation,
         {-CinemaAuthoredScreen[0], -CinemaAuthoredScreen[1], -CinemaAuthoredScreen[2]}});
}

std::string CinemaEnvironmentCommand(const std::vector<std::string>& args) {
    std::scoped_lock lock(control_mutex);
    if (args.empty() || (args.size() == 1 && args[0] == "status"))
        return fmt::format("world={}\n{}", world_override ? *world_override :
            Property("debug.shadps4.xr_cinema_world", "tv-lounge"), diagnostics);
    if (args.size() == 2 && args[0] == "world") {
        world_override = args[1];
        return "cinema world queued: " + args[1];
    }
    return "Usage: xr_cinema status | world <tv-lounge|dusk-terrace|dark-room|seaside|void|off>";
}

struct CinemaEnvironment::Impl {
    spatial::xr::XrSceneVulkanLayer layer;
    std::unique_ptr<spatial::scene::BasicScene> scene;
    std::unique_ptr<spatial::world::World> world;
    std::string key;
    std::array<float, 3> ambient{};
    std::vector<XrCompositionLayerProjectionView> projection_views;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrPosef rendered_origin{xm::IdentityPose()};
    uint64_t frames{}, reused{};
    Clock::time_point last_diagnostic{};
    ~Impl() {
        if (auto* view = layer.View()) { view->SetGi(nullptr); view->SetIbl(nullptr); }
        world.reset();
        scene.reset();
        layer.Destroy();
        std::scoped_lock lock(control_mutex);
        diagnostics = "cinema environment stopped";
    }
};

CinemaEnvironment::CinemaEnvironment() : impl(std::make_unique<Impl>()) {}
CinemaEnvironment::~CinemaEnvironment() = default;
const std::string& CinemaEnvironment::WorldKey() const { return impl->key; }
const XrCompositionLayerProjection* CinemaEnvironment::Layer() const { return &impl->projection; }

bool CinemaEnvironment::Create(XrSession session, const spatial::xr::SwapchainFunctions& functions,
                               const spatial::xr::XrImguiVulkanBinding& binding, uint32_t w,
                               uint32_t h, const std::string& cache_dir, const std::string& key) {
    auto& p = *impl;
    p.key = key;
    const auto started = Clock::now();
    const fs::path root = fs::path(cache_dir) / "xr-cinema";
    size_t files{}, bytes{};
    ExtractAssets(root, files, bytes);
    const fs::path document = root / (key + ".world.json");
    spatial::world::WorldAsset asset;
    std::string error;
    if (!spatial::world::ReadWorldFile(document, asset, error))
        throw std::runtime_error("cinema world " + key + ": " + error);
    spatial::xr::XrSceneDeviceFeatures features;
    features.keep_model_geometry = asset.environment.gi; // voxel GI reads CPU geometry
    if (!p.layer.Create(session, functions, binding, w, h, 2, 128,
                        (fs::path(cache_dir) / "xr-cinema-shaders").string(), root.string(), features))
        throw std::runtime_error("cinema Lite Engine: " + p.layer.RendererError());
    p.layer.SetDepthRange(.03f, 400.f); // the sky dome sits at 150-350 m
    p.scene = std::make_unique<spatial::scene::BasicScene>("xr_cinema");
    p.world = std::make_unique<spatial::world::World>(*p.scene, &p.layer.Engine());
    p.world->Environment() = asset.environment;
    if (!p.world->Load(asset.graph, error))
        throw std::runtime_error("cinema world load: " + error);
    for (const char* name : kRuntimeOwned)
        for (auto object : p.world->FindByName(name)) p.world->Destroy(object);
    const auto& e = asset.environment;
    for (unsigned i = 0; i < 3; ++i)
        p.ambient[i] = (e.sky_colour[i] + e.ground_colour[i]) * .5f * e.ambient_intensity;
    // The World's own lights carry the room; no extra directional light.
    p.layer.SetEnvironment({.exposure = e.exposure, .light_intensity = 0.f});
    p.world->ApplyEnvironment(*p.layer.View());
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::scoped_lock lock(control_mutex);
    diagnostics = fmt::format("created world={} objects={} assets={} files/{:.1f} MiB eye={}x{} gi={} load_ms={:.0f}",
                              key, p.world->ObjectCount(), files, bytes / 1048576.0, w, h, e.gi, ms);
    return true;
}

bool CinemaEnvironment::Render(std::span<const XrView> eyes, XrSpace space, const XrPosef& screen_pose) {
    auto& p = *impl;
    if (eyes.size() != 2) return false;
    const auto origin = CinemaAuthoredOrigin(screen_pose);
    const auto views = StatusProjection::LocalViews({eyes[0], eyes[1]}, origin);
    p.scene->TickFrame();
    spatial::xr::XrSceneLighting lighting;
    lighting.ambient_colour = p.ambient;
    if (!p.layer.Render(views, *p.scene, lighting)) return false;
    if (!p.layer.LastRenderReused()) p.rendered_origin = origin;
    else ++p.reused;
    if (!p.layer.FillProjectionLayer(space, p.projection_views, p.projection)) return false;
    // Back to LOCAL space with the origin of the render that made the pixels.
    for (auto& view : p.projection_views)
        view.pose = StatusProjection::ToWorld(p.rendered_origin, view.pose);
    p.projection.layerFlags = 0; // opaque bottom layer
    ++p.frames;
    const auto now = Clock::now();
    if (now - p.last_diagnostic >= std::chrono::seconds(1)) {
        const auto* gi = p.world->Gi();
        std::scoped_lock lock(control_mutex);
        const auto head = diagnostics.substr(0, diagnostics.find('\n'));
        diagnostics = fmt::format("{}\nframes={} reused={} gi_voxels={} gi_probes={} origin={:.3f},{:.3f},{:.3f} {}",
            head, p.frames, p.reused, gi ? gi->Stats().voxels : 0, gi ? gi->Stats().probes : 0,
            origin.position.x, origin.position.y, origin.position.z, p.layer.RendererError());
        p.last_diagnostic = now;
    }
    return true;
}
} // namespace Vulkan::OpenXr
