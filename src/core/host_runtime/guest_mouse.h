// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <string_view>

namespace Core::HostRuntime {
// Android does not expose a PS4 mouse provider. These entry points are
// admitted only so the guest receives the desktop-compatible named error and
// can take its optional-input fallback path; no host mouse state is invented.
inline constexpr std::string_view MouseNids[]{
    "cAnT0Rw-IwU", "RaqxZIf6DvE", "Qs0Wulgl7U", "x8qnXqh-tiM",
    "crkFfp-cmFo", "ghLUU2Z5Lcg", "6aANndpS0Wo"};
inline bool IsMouseNid(std::string_view nid) {
    return std::find(std::begin(MouseNids), std::end(MouseNids), nid) != std::end(MouseNids);
}
} // namespace Core::HostRuntime
