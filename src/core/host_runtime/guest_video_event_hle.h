// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/videoout/videoout_error.h"

namespace Core::HostRuntime {
inline constexpr std::array<std::string_view, 3> VideoEventGetterNids{
    "U2JJtSqNKZI", "rWUTcKdkUzQ", "Mt4QHHkxkOc"};
inline u32 DispatchVideoEventGetter(GuestCpu::GuestAddressSpace& space,
                                    std::string_view nid, u64 input, u64 output) {
    Libraries::Kernel::OrbisKernelEvent event{};
    if (!space.ReadData({input}, std::as_writable_bytes(std::span{&event, 1})))
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    using namespace Libraries::VideoOut;
    if (nid == "U2JJtSqNKZI") return sceVideoOutGetEventId(&event);
    if (nid == "Mt4QHHkxkOc") return sceVideoOutGetEventCount(&event);
    auto pin = space.AcquireDataSpan({{output}, sizeof(s64)}, true);
    if (!pin) return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    s64 value{};
    const auto result = sceVideoOutGetEventData(&event, &value);
    if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
    return result;
}
} // namespace Core::HostRuntime
