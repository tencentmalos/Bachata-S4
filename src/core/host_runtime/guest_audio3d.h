// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <stop_token>
#include <string_view>
#include "common/types.h"
namespace Core::GuestCpu {
class GuestAddressSpace;
}
namespace Core::HostRuntime {
class GuestAudio;
inline constexpr std::string_view Audio3dNids[]{
    "UmCvjSmuZIw", "WW1TS2iz5yc", "Im+jOoa5WAI", "XeDDK0xJWQA", "OyVqOeVNtSk", "jO2tec4dJ2M",
    "1HXxo-+1qCw", "4uyHN9q4ZeU", "V1FBFpNIAzk", "9tEwE0GV0qo", "lw0qrdSjZt8", "VEVhZ9qd4ZY",
    "ZOGrxWLgQzE", "YaaDbDwKpFM", "Yq9bfUQ0uJg", "9ZA23Ia46Po", "UHFOgVNz0kk"};
bool IsAudio3dNid(std::string_view nid);
class GuestAudio3d {
public:
    GuestAudio3d(GuestCpu::GuestAddressSpace& space, GuestAudio& audio);
    ~GuestAudio3d();
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& args, std::stop_token stop = {});
    void RequestStop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
