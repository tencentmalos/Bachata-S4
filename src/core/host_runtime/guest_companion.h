// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/companion/companion_httpd.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
// The desktop provider has no second-screen transport. Preserve its disconnected
// behavior without ever handing a guest callback or pointer to native code.
inline constexpr std::array<std::string_view, 8> CompanionOfflineNids{
    "ykNpWs3ktLY", "OA6FbORefbo", "Vku4big+IYM", "-0c9TCTwnGs",
    "k7F0FcDM-Xc", "0SCgzfVQHpo", "+-du9tWgE9s", "xweOi2QT-BE"};
inline bool IsCompanionOfflineNid(std::string_view nid) {
    for (auto candidate : CompanionOfflineNids) if (candidate == nid) return true;
    return false;
}
inline u32 DispatchCompanionOffline(GuestCpu::GuestAddressSpace& space,
                                    std::string_view nid, u64 output) {
    using namespace Libraries::CompanionHttpd;
    if (nid == "ykNpWs3ktLY") return sceCompanionHttpdInitialize();
    if (nid == "OA6FbORefbo") return sceCompanionHttpdInitialize2();
    if (nid == "-0c9TCTwnGs") return sceCompanionHttpdRegisterRequestCallback2(nullptr, nullptr);
    if (nid == "k7F0FcDM-Xc") return sceCompanionHttpdStart();
    if (nid == "0SCgzfVQHpo") return sceCompanionHttpdStop();
    if (nid == "+-du9tWgE9s") return sceCompanionHttpdTerminate();
    if (nid == "xweOi2QT-BE") return sceCompanionHttpdUnregisterRequestCallback();
    if (nid != "Vku4big+IYM") return ORBIS_KERNEL_ERROR_ENOSYS;
    OrbisCompanionHttpdEvent event{};
    const auto result = sceCompanionHttpdGetEvent(&event);
    // The desktop function only updates this field on NO_EVENT.
    if (!space.WriteData({output}, std::as_bytes(std::span{&event.event, 1})))
        return ORBIS_KERNEL_ERROR_EFAULT;
    return result;
}
} // namespace Core::HostRuntime
