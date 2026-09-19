// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"

namespace Libraries::VrTracker {

inline bool PlayAreaWarningInfoShapeValid(const OrbisVrTrackerPlayAreaWarningInfo* info) {
    if (!info || info->size != sizeof(*info))
        return false;
    const auto all_zero = [](const auto& values) {
        for (const auto value : values) {
            if (value != 0)
                return false;
        }
        return true;
    };
    return all_zero(info->reserved0) && all_zero(info->reserved1) &&
           all_zero(info->reserved2) && all_zero(info->reserved3) &&
           all_zero(info->reserved4);
}

// Firmware 11.00 libSceVrTracker+0xb080 validates initialization, size and
// reserved fields before copying its cached warning record. We have no such
// provider yet: NOT_SUPPORTED is emulator policy, not a firmware no-camera
// branch. Do not report success or overwrite the caller's output fields.
inline s32 PlayAreaWarningInfoNoProvider(bool initialized,
                                        const OrbisVrTrackerPlayAreaWarningInfo* info) {
    if (!initialized)
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    if (!PlayAreaWarningInfoShapeValid(info))
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    return ORBIS_VR_TRACKER_ERROR_NOT_SUPPORTED;
}

} // namespace Libraries::VrTracker
