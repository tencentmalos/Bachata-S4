// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <stop_token>
#include "core/libraries/audio/audioout.h"
namespace Libraries::AudioOut {
// Shared by the AAudio backend and focused fault-injection tests. The writer is
// called with a monotonically advancing frame offset, never a replayed prefix.
struct AudioWriteResult {
    int error{};
    u32 accepted{};
    bool cancelled{}, timed_out{};
};
template <class Write, class Recover>
AudioWriteResult TransferAudioFrames(u32 frames, std::stop_token stop, Write write,
                                     Recover recover) {
    AudioWriteResult result;
    bool retried{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (result.accepted < frames) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            result.timed_out = true;
            return result;
        }
        const int written = write(result.accepted, frames - result.accepted,
                                  std::min<int64_t>(remaining, 10000000));
        if (written < 0) {
            if (!retried && !stop.stop_requested() && recover(written)) {
                retried = true;
                continue;
            }
            result.error = written;
            return result;
        }
        if (u32(written) > frames - result.accepted) {
            result.error = -1;
            return result;
        }
        result.accepted += written;
    }
    return result;
}
inline void ConvertAudioFrames(const AudioFormatInfo& info, u32 frames, const void* input,
                               const std::array<int, 8>& volume, float slider,
                               std::span<float> output) {
    for (u32 frame = 0; frame < frames; ++frame)
        for (u32 channel = 0; channel < info.num_channels; ++channel) {
            const size_t index = size_t(frame) * info.num_channels + info.channel_layout[channel];
            const float value = info.is_float ? static_cast<const float*>(input)[index]
                                              : static_cast<const s16*>(input)[index] / 32768.0f;
            const float gain =
                std::clamp(volume[channel], 0, 32768) / 32768.0f * std::clamp(slider, 0.0f, 1.0f);
            output[size_t(frame) * info.num_channels + channel] =
                std::isfinite(value) ? std::clamp(value * gain, -1.0f, 1.0f) : 0;
        }
}
} // namespace Libraries::AudioOut
