// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

namespace Shader {

// Diagnostic shader experiments must name one title. A device-wide boolean left
// by another game must not change this game's shader profile or cached pipelines.
constexpr bool IsTitleShaderExperimentEnabled(std::string_view requested_title,
                                              std::string_view current_title) {
    if (requested_title.size() != 9 || !requested_title.starts_with("CUSA") ||
        requested_title != current_title) {
        return false;
    }
    for (const char c : requested_title.substr(4)) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

} // namespace Shader
