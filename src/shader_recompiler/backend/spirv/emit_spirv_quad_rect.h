// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>
#include "common/types.h"

namespace Shader {
struct Info;
struct Profile;
} // namespace Shader

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

// Auxiliary stages forward the producing vertex shader's actual locations.
// Fragment state may be absent, stale, sparse, defaulted or remapped.
[[nodiscard]] std::vector<u32> AuxiliaryVaryingLocations(const Info& vertex,
                                                         const Profile& profile);
[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      std::span<const u32> locations,
                                                      bool depth_clip_passthrough = false);

} // namespace Shader::Backend::SPIRV
