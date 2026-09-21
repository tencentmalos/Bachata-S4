// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include "core/libraries/audio/audioin.h"
#include "core/libraries/audio/audioin_error.h"
namespace Core::HostRuntime {
inline constexpr std::array<std::string_view, 4> AudioInputNids{
    "5NE8Sjc7VC8", "Jh6WbHhnI68", "LozEOU8+anM", "BohEAQ7DlUE"};
inline bool IsAudioInputNid(std::string_view nid) {
    for (auto candidate : AudioInputNids) if (candidate == nid) return true;
    return false;
}
// Android's shared NullAudioIn backend does not publish ports. Keep its real
// validation/error contract; no silence buffer, fake handle, or guest pointer
// reaches a native audio callback. A future microphone provider needs a checked
// data bridge before this admission policy can change.
inline u32 DispatchAudioInput(std::string_view nid, const std::array<u64, 6>& a) {
    using namespace Libraries::AudioIn;
    if (nid == "5NE8Sjc7VC8") return sceAudioInOpen(s32(a[0]), a[1], a[2], a[3], a[4], a[5]);
    if (nid == "Jh6WbHhnI68") return sceAudioInClose(s32(a[0]));
    if (nid == "BohEAQ7DlUE") return sceAudioInGetSilentState(s32(a[0]));
    return InputWithoutDevice(s32(a[0]), a[1] != 0);
}
} // namespace Core::HostRuntime
