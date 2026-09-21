// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/videoout/videoout_error.h"

namespace Core::HostRuntime {
struct GuestVideoModeRequest {
    Libraries::VideoOut::Mode mode{};
    std::array<u8, 16> options{};
};

inline u32 ReadGuestVideoMode(GuestCpu::GuestAddressSpace& space,
                             const std::array<u64, 6>& args, GuestVideoModeRequest& out) {
    // Firmware 11.00 +0xd780 accepts size_options 0 or 16 independently
    // of the pointer. +0xdc19 reads all 16 bytes when non-null, otherwise
    // supplies zero defaults. The SDK wrapper passes 16 even for nullptr.
    if (u32(args[1]) || u32(args[4]) != sizeof(out.mode) ||
        (u32(args[5]) != 0 && u32(args[5]) != out.options.size()) || !args[2])
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    GuestVideoModeRequest local{};
    // ReadData pins the complete input during each copy. Only local values
    // cross into the provider, including when guest memory is read-only.
    if (!space.ReadData({args[2]}, std::as_writable_bytes(std::span{&local.mode, 1})) ||
        (args[3] && !space.ReadData({args[3]}, std::as_writable_bytes(std::span{local.options}))))
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    if (local.mode.size != sizeof(local.mode))
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    out = local;
    return 0;
}
} // namespace Core::HostRuntime
