// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <string_view>
#include "common/types.h"

namespace Core::GuestCpu {
class GuestAddressSpace;
}
namespace Core::HostRuntime {
inline constexpr std::string_view Videodec2Nids[]{
    "RnDibcGCPKw", "eD+X2SmxUt4", "UvtA3FAiF4Y", "qqMCwlULR+E", "CNNRoRYd8XI", "jwImxXRGSKA",
    "852F5+q6+iM", "l1hXwscLuCY", "wJXikG6QFN8", "NtXRa3dRzU0", "kjrLbcyhEiw", "7M+1UFqWOAI"};
bool IsVideodec2Nid(std::string_view nid);

// Session-owned handles; guest addresses never become decoder pointers. The
// codec consumes a padded input copy and writes only into admitted output spans.
class GuestVideodec2 {
public:
    explicit GuestVideodec2(GuestCpu::GuestAddressSpace&);
    ~GuestVideodec2();
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& args);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
