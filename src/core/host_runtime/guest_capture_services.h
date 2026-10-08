// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include <string>
#include <mutex>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/video_recording/video_recording_error.h"

namespace Core::HostRuntime {
// Guest capture policy is session-local and separate from host diagnostics.
// Configuration does not create a capture file or imply a recording provider.
struct GuestCapturePolicy {
    std::mutex mutex;
    bool screenshot_enabled{true}, notification_enabled{true};
    std::string overlay_path;
    u32 overlay_x{}, overlay_y{}, overlay_origin{1};
};
enum class CaptureServiceCall { ScreenControl, Overlay, OverlayOrigin, ShareInit, ShareAbsent, RecordingAbsent };
struct CaptureServiceEntry {
    std::string_view nid, suffix;
    CaptureServiceCall call;
};
inline constexpr std::string_view ScreenshotSuffix =
    "#libSceScreenShot#1#libSceScreenShot#Function";
inline constexpr std::string_view ShareUtilitySuffix =
    "#libSceShareUtility#1#libSceShareUtility#Function";
inline constexpr std::string_view VideoRecordingSuffix =
    "#libSceVideoRecording#1#libSceVideoRecording#Function";
inline constexpr std::array<CaptureServiceEntry, 16> CaptureServiceEntries{{
    {"Fc8qxlKINYQ", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // SetInfo
    {"bGYkY6q3bIw", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // QueryMemSize2
    {"s28dalBwp2g", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // Open2
    {"KHvkPQJDMLk", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // Close
    {"tWoe9IlGAhs", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // Start
    {"OOFxrMY+mfI", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // Stop
    {"fZJQzFK4Gv4", VideoRecordingSuffix, CaptureServiceCall::RecordingAbsent}, // GetStatus
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
                                  u32 sdk, GuestCapturePolicy* policy = nullptr) {
    using namespace GuestCpu;
    constexpr u32 no_provider = u32(ORBIS_KERNEL_ERROR_ENOSYS);
    // The module can be loaded, but Android supplies no PS4 recording service.
    // Return its explicit unsupported error before reading or writing arguments.
    if (entry.call == CaptureServiceCall::RecordingAbsent)
        return u32(ORBIS_VIDEO_RECORDING_ERROR_UNSUPPORTED);
    if (entry.call == CaptureServiceCall::ScreenControl) {
        if (!policy) return no_provider;
        std::scoped_lock lock(policy->mutex);
        if (entry.nid == "tIYf0W5VTi8") policy->screenshot_enabled = false;
        if (entry.nid == "2xxUtuC-RzE") policy->screenshot_enabled = true;
        if (entry.nid == "ysfza71rm9M") policy->notification_enabled = false;
        if (entry.nid == "BDUaqlVdSAY") policy->notification_enabled = true;
        return 0;
    }
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
    // Copy the bounded path before publishing policy. No guest pointer outlives
    // the call; actual capture remains unavailable until a provider consumes it.
    std::string path;
    for (u64 offset = 0; offset < 1024; ++offset) {
        char value{};
        if (!space.ReadData({a[0] + offset}, std::as_writable_bytes(std::span{&value, 1})))
            return invalid;
        if (!value) {
            if (!policy) return no_provider;
            std::scoped_lock lock(policy->mutex);
            policy->overlay_path = std::move(path);
            policy->overlay_x = u32(a[1]);
            policy->overlay_y = u32(a[2]);
            policy->overlay_origin = entry.call == CaptureServiceCall::OverlayOrigin ? u32(a[3]) : 1;
            return 0;
        }
        path.push_back(value);
    }
    return invalid;
}
} // namespace Core::HostRuntime
