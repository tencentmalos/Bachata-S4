// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include "common/types.h"

namespace Core::HostRuntime {
// The Android host has no ShellCore broadcast/IPMI provider. Firmware 11.00
// Initialize validates its scalar workspace size, then maps service ENOENT to
// 0x80a00007. Failed initialization leaves the client absent. Each admitted
// operation below checks that client before touching any caller pointer.
// Keep that cold path, including untouched outputs and retry/teardown errors;
// never publish an initialized handle, callbacks, messages or a fake broadcast.
// See monster-hunter-media-20260922.md for the per-export firmware audit.
inline constexpr u32 LiveStreamingInvalidArgument = 0x80a00002;
inline constexpr u32 LiveStreamingNotInitialized = 0x80a00004;
inline constexpr u32 LiveStreamingServiceUnavailable = 0x80a00007;
inline constexpr std::array<std::string_view, 21> LiveStreamingUnavailableNids{
    "FcHBfHjFXkA", // sceGameLiveStreamingClearPresetSocialFeedbackCommands
    "lZ2Sd0uEvpo", // sceGameLiveStreamingClearSocialFeedbackMessages
    "6c2zGtThFww", // sceGameLiveStreamingClearSpoilerTag
    "dWM80AX39o4", // sceGameLiveStreamingEnableLiveStreaming
    "lK8dLBNp9OE", // sceGameLiveStreamingGetCurrentStatus2
    "OIIm19xu+NM", // sceGameLiveStreamingGetProgramInfo
    "PMx7N4WqNdo", // sceGameLiveStreamingGetSocialFeedbackMessages
    "yeQKjHETi40", // sceGameLiveStreamingGetSocialFeedbackMessagesCount
    "kvYEw2lBndk", // sceGameLiveStreamingInitialize
    "K0QxEbD7q+c", // sceGameLiveStreamingPermitLiveStreaming
    "-EHnU68gExU", // sceGameLiveStreamingPermitServerSideRecording
    "hggKhPySVgI", // sceGameLiveStreamingPostSocialMessage
    "3PSiwAzFISE", // sceGameLiveStreamingSetCameraFrameSetting
    "QmQYwQ7OTJI", // sceGameLiveStreamingSetInvitationSessionId
    "q-kxuaF7URU", // sceGameLiveStreamingSetMaxBitrate
    "hUY-mSOyGL0", // sceGameLiveStreamingSetMetadata
    "ycodiP2I0xo", // sceGameLiveStreamingSetPresetSocialFeedbackCommands
    "x6deXUpQbBo", // sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription
    "ZuX+zzz2DkA", // sceGameLiveStreamingSetSpoilerTag
    "MLvYI86FFAo", // sceGameLiveStreamingSetStandbyScreenResource
    "9yK6Fk8mKOQ", // sceGameLiveStreamingTerminate
};
inline bool IsLiveStreamingUnavailableNid(std::string_view nid) {
    for (auto candidate : LiveStreamingUnavailableNids)
        if (candidate == nid) return true;
    return false;
}
inline bool AdmitsLiveStreamingUnavailable(std::string_view nid, std::string_view suffix) {
    return IsLiveStreamingUnavailableNid(nid) &&
           suffix == "#libSceGameLiveStreaming#1#libSceGameLiveStreaming#Function";
}
inline u32 DispatchLiveStreamingUnavailable(std::string_view nid, const std::array<u64, 6>& a) {
    if (nid == "kvYEw2lBndk")
        return a[0] == 0x4000 ? LiveStreamingServiceUnavailable : LiveStreamingInvalidArgument;
    return LiveStreamingNotInitialized;
}
} // namespace Core::HostRuntime
