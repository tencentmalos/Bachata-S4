// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
// Guest ShellCore screenshot/share clients are separate from host diagnostic
// scrcpy capture. No service, capture file, or successful policy change is
// fabricated. ENOSYS is the host provider boundary, not a firmware IPC result.
enum class CaptureServiceCall { ScreenControl, Overlay, OverlayOrigin, ShareInit, ShareAbsent };
struct CaptureServiceEntry {
    std::string_view nid, suffix;
    CaptureServiceCall call;
};
inline constexpr std::string_view ScreenshotSuffix =
    "#libSceScreenShot#1#libSceScreenShot#Function";
inline constexpr std::string_view ShareUtilitySuffix =
    "#libSceShareUtility#1#libSceShareUtility#Function";
inline constexpr std::array<CaptureServiceEntry, 9> CaptureServiceEntries{{
    {"tIYf0W5VTi8", ScreenshotSuffix, CaptureServiceCall::ScreenControl}, // Disable
    {"2xxUtuC-RzE", ScreenshotSuffix, CaptureServiceCall::ScreenControl}, // Enable
    {"ysfza71rm9M", ScreenshotSuffix, CaptureServiceCall::ScreenControl}, // DisableNotification
    {"BDUaqlVdSAY", ScreenshotSuffix, CaptureServiceCall::ScreenControl}, // EnableNotification
    {"ahHhOf+QNkQ", ScreenshotSuffix, CaptureServiceCall::Overlay},
    {"73WQ4Jj0nJI", ScreenshotSuffix, CaptureServiceCall::OverlayOrigin},
    {"j7DlalBzHh8", ShareUtilitySuffix, CaptureServiceCall::ShareInit},   // InitializeEx2
    {"DUWhxkyVPj4", ShareUtilitySuffix, CaptureServiceCall::ShareAbsent}, // Terminate
    {"8hZ2EEl2Tto", ShareUtilitySuffix, CaptureServiceCall::ShareAbsent}, // OpenShareMenuDefault
}};
inline const CaptureServiceEntry* FindCaptureService(std::string_view nid) {
    for (const auto& entry : CaptureServiceEntries)
        if (entry.nid == nid)
            return &entry;
    return nullptr;
}
inline bool AdmitsCaptureService(std::string_view nid, std::string_view suffix) {
    const auto* entry = FindCaptureService(nid);
    return entry && entry->suffix == suffix;
}
inline u32 DispatchCaptureService(GuestCpu::GuestAddressSpace& space,
                                  const CaptureServiceEntry& entry, const std::array<u64, 6>& a,
                                  u32 sdk) {
    using namespace GuestCpu;
    constexpr u32 no_provider = u32(ORBIS_KERNEL_ERROR_ENOSYS);
    if (entry.call == CaptureServiceCall::ScreenControl)
        return no_provider;
    if (entry.call == CaptureServiceCall::ShareAbsent)
        return 0x81170102;
    if (entry.call == CaptureServiceCall::ShareInit) {
        // Firmware InitializeEx2(type, workspace_size, callback_priority, arg).
        // Cold initialization checks these scalars before creating its IPMI
        // client; callback arguments are consumed only after client success.
        return a[0] == 1 && a[1] >= 0x20000 ? no_provider : 0x81170003;
    }
    constexpr u32 invalid = 0x80be0001;
    if (entry.call == CaptureServiceCall::OverlayOrigin && (u32(a[3]) - 1u > 8u))
        return invalid;
    const u32 width = sdk < 0x03500000 ? 1920 : 3840;
    const u32 height = sdk < 0x03500000 ? 1080 : 2160;
    if (!a[0] || a[0] > UINT64_MAX - 1023 || u32(a[1]) >= width || u32(a[2]) >= height)
        return invalid;
    // Match the bounded firmware path string check. The provider is required
    // before opening the file, so do not access storage or change capture state.
    for (u64 offset = 0; offset < 1024; ++offset) {
        char value{};
        if (!space.ReadData({a[0] + offset}, std::as_writable_bytes(std::span{&value, 1})))
            return invalid;
        if (!value)
            return no_provider;
    }
    return invalid;
}
} // namespace Core::HostRuntime
