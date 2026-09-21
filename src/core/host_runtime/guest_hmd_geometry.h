// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"

namespace Core::HostRuntime {
inline constexpr char HmdEyeOffsetNid[] = "BWY-qKM5hxE";
inline u32 GuestHmdEyeOffsets(GuestCpu::GuestAddressSpace& space, s32 handle,
                             u64 left_address, u64 right_address) {
    using namespace Libraries::Hmd;
    OrbisHmdEyeOffset left{}, right{};
    const auto result = sceHmdGet2DEyeOffset(handle, &left, &right);
    if (result) return static_cast<u32>(result);
    // Admit both outputs together: a missing right eye must never leave a
    // half-written left eye. No guest pointer crosses into the native provider.
    // Firmware 11.00 +0x5677..0x56b7 writes only the three float fields;
    // preserve caller-owned reserved bytes after the 12-byte payload.
    constexpr auto payload_size = offsetof(OrbisHmdEyeOffset, reserve);
    static_assert(payload_size == 12);
    const std::array<GuestCpu::GuestAddressSpace::DataRequest, 2> requests{{
        {{{left_address}, payload_size}, GuestCpu::GuestPermission::Write},
        {{{right_address}, payload_size}, GuestCpu::GuestPermission::Write}}};
    if (!left_address || !right_address) return ORBIS_HMD_ERROR_PARAMETER_NULL;
    auto pins = space.AcquireDataBatch(requests);
    if (!pins) return ORBIS_HMD_ERROR_PARAMETER_NULL;
    std::memcpy(pins.Value()[0].WritableBytes().data(), &left, payload_size);
    std::memcpy(pins.Value()[1].WritableBytes().data(), &right, payload_size);
    return 0;
}
} // namespace Core::HostRuntime
