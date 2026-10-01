// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include "spatial/xr/SwapchainFunctions.h"
#include "spatial/xr/XrImguiVulkanLayer.h"

namespace Vulkan::OpenXr {
// The cinema environment: a Lite Editor World (assets/xr/cinema, packaged
// under xr/cinema) drawn as the bottom, opaque projection layer behind the
// game's quad layer. The World is authored with the floor at y=0, the seated
// eye at (0,1.45,0) and the screen centre at CinemaAuthoredScreen; it is
// placed so that point coincides with the runtime's cinema quad pose.
// GPU and scene objects stay on the XR frame thread.
inline constexpr std::array<float, 3> CinemaAuthoredScreen{0.f, 1.55f, -3.0f};
std::string CinemaEnvironmentCommand(const std::vector<std::string>& args);
// The World to show ("off" = none), read at session start and on change.
std::string CinemaWorld();
// The authored origin (floor under the seated eye) in LOCAL space for a
// cinema quad pose.
XrPosef CinemaAuthoredOrigin(const XrPosef& screen_pose);

class CinemaEnvironment {
public:
    CinemaEnvironment();
    ~CinemaEnvironment();
    // Extracts the packaged World and models into `cache_dir` (once per
    // content size) and loads `world` (a key such as "tv-lounge").
    bool Create(XrSession session, const spatial::xr::SwapchainFunctions& functions,
                const spatial::xr::XrImguiVulkanBinding& binding, uint32_t eye_width,
                uint32_t eye_height, const std::string& cache_dir, const std::string& world);
    const std::string& WorldKey() const;
    bool Render(std::span<const XrView> views, XrSpace space, const XrPosef& screen_pose);
    const XrCompositionLayerProjection* Layer() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan::OpenXr
