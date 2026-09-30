// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <string>
#include "common/types.h"
// Keep Foundation math's private assertion macro out of client headers (JSON).
#pragma push_macro("assert_invariant")
#include "spatial/foveation/Foveation.h"
#pragma pop_macro("assert_invariant")
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
namespace HostPasses {
enum class SpatialFilter : u32 { Off, Fsr1, Sgsr1 };
enum class XrOutputResolution : u32 { Recommended, High, Maximum };
struct SpatialOptions {
    SpatialFilter filter{SpatialFilter::Fsr1};
    spatial::foveation::Mode foveation{spatial::foveation::Mode::Off};
    spatial::foveation::Level level{spatial::foveation::Level::Low};
    u32 sharpness{50};
    bool debug{};
    XrOutputResolution output_resolution{XrOutputResolution::Recommended};
};
void SetSpatialOptions(SpatialOptions options);
SpatialOptions GetSpatialOptions();
std::string SpatialStatus();
void PublishGazeStatus(bool supported, bool fresh, uint64_t valid, uint64_t invalid);
void PublishXrOutputExtent(vk::Extent2D recommended, vk::Extent2D limit, vk::Extent2D actual);
struct FoveatedEye {
    spatial::foveation::AtlasUv center{};
    spatial::foveation::Profile profile{};
    bool enabled{}, tracked{};
};
// Each slot belongs to an acquired Presenter frame/eye. The caller waits for
// that frame's fence before reuse; no mutable scratch is shared between eyes.
class SpatialUpscalePass {
public:
    explicit SpatialUpscalePass(const Instance& instance);
    ~SpatialUpscalePass();
    vk::ImageView Render(Scheduler& scheduler, u32 slot, vk::ImageView input,
                         vk::Extent2D input_extent, std::array<float, 4> uv,
                         vk::Extent2D output_extent, SpatialOptions options,
                         FoveatedEye foveation = {}, bool encoded_input = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace HostPasses
} // namespace Vulkan
