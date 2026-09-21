// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/systemservice_error.h"

namespace Core::HostRuntime {
inline constexpr char DisplaySafeAreaNid[] = "1n37q1Bvc5Y";

inline u32 GuestDisplaySafeAreaInfo(GuestCpu::GuestAddressSpace& space, u64 address) {
    using namespace Libraries::SystemService;
    if (!address)
        return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;
    // Pin the whole output before calling the native provider. Guest pointers
    // never escape into desktop HLE; no validate-then-write remapping window.
    auto pin = space.AcquireDataSpan(
        {GuestCpu::GuestAddress{address}, sizeof(OrbisSystemServiceDisplaySafeAreaInfo)}, true);
    if (!pin)
        return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;
    OrbisSystemServiceDisplaySafeAreaInfo info{};
    const auto result = sceSystemServiceGetDisplaySafeAreaInfo(&info);
    if (result == 0)
        std::memcpy(pin.Value().WritableBytes().data(), &info, sizeof(info));
    return static_cast<u32>(result);
}
} // namespace Core::HostRuntime
