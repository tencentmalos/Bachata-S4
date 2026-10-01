// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <optional>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::SPIRV {
// Ordinary flat arrays carry the three original vertices. Unlike PerVertexKHR,
// these arrays consume three locations, explicitly allocated away from other inputs.
struct SoftwareInterpolationLayout {
    std::array<u32, IR::NumParams> locations{};
    std::optional<u32> smooth, linear, linear_sample;
    u32 location_count{};
    bool needed{};
};
SoftwareInterpolationLayout MakeSoftwareInterpolationLayout(const Info& fragment,
                                                            const HwFragmentRuntimeInfo& fs);
std::vector<u32> EmitSoftwareInterpolationGeometry(const Info& vertex, const RuntimeInfo& vs,
                                                   const Info& fragment,
                                                   const HwFragmentRuntimeInfo& fs,
                                                   u32 max_output_components,
                                                   u32 max_total_output_components);
} // namespace Shader::Backend::SPIRV
