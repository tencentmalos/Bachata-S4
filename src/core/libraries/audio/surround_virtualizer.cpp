// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "core/libraries/audio/surround_virtualizer.h"

namespace Libraries::AudioOut {

namespace {

constexpr float Pi = 3.14159265358979323846f;

// Where the speakers of a 7.1 room stand, as degrees to the right of straight ahead: front
// left and right, centre, the surround pair at the sides and the pair behind. Guest logical
// order is L R C LFE Ls Rs Le Re; titles pan a sound 80 degrees to the right mostly into Rs,
// so Ls/Rs is the 5.1 surround pair at about 110 degrees and Le/Re the pair 7.1 adds behind.
constexpr std::array<float, 7> SpeakerAngle = {-30.0f, 30.0f, 0.0f, -110.0f,
                                               110.0f, -150.0f, 150.0f};
// The matching guest logical channels (index 3 is the LFE).
constexpr std::array<u32, 7> SpeakerChannel = {0, 1, 2, 4, 5, 6, 7};
constexpr u32 LfeChannel = 3;

// Seven speakers playing at once are louder than any one of them.
constexpr float Headroom = 0.8f;
constexpr float LfeLevel = 0.5f;
constexpr float LfeCutoff = 160.0f;

std::atomic<int> mode{-1};

} // namespace

SurroundVirtualizer::SurroundVirtualizer(u32 max_frames, u32 sample_rate_)
    : sample_rate{static_cast<float>(sample_rate_)} {
    for (auto& channel : planar) {
        channel.resize(max_frames);
    }
}

void SurroundVirtualizer::Process(const AudioFormatInfo& info, u32 frames, const void* input,
                                  const std::array<int, 8>& volume, float slider,
                                  std::span<float> output) noexcept {
    if (output.size() < size_t(frames) * 2 || info.num_channels != 8) {
        return;
    }
    frames = std::min<u32>(frames, static_cast<u32>(planar[0].size()));
    const float master = std::isfinite(slider) ? std::clamp(slider, 0.0f, 1.0f) : 0.0f;
    for (u32 ch = 0; ch < 8; ++ch) {
        const float gain = std::clamp(volume[ch], 0, 32768) / 32768.0f * master;
        float* target = planar[ch].data();
        const int slot = std::clamp(info.channel_layout[ch], 0, 7);
        for (u32 frame = 0; frame < frames; ++frame) {
            const size_t index = size_t(frame) * 8 + slot;
            // Guest ABI does not guarantee natural alignment for PCM buffers.
            float sample{};
            if (info.is_float) {
                std::memcpy(&sample, static_cast<const u8*>(input) + index * 4, 4);
            } else {
                s16 integer{};
                std::memcpy(&integer, static_cast<const u8*>(input) + index * 2, 2);
                sample = integer / 32768.0f;
            }
            target[frame] = std::isfinite(sample) ? std::clamp(sample * gain, -8.0f, 8.0f) : 0.0f;
        }
    }

    float* out = output.data();
    std::fill_n(out, size_t(frames) * 2, 0.0f);
    for (u32 speaker = 0; speaker < NumSpeakers; ++speaker) {
        const float angle = SpeakerAngle[speaker] * Pi / 180.0f;
        Audio3d::ObjectPlacement placement;
        placement.x = std::sin(angle);
        placement.z = -std::cos(angle); // the spatializer's +Z is behind the listener
        placement.gain = Headroom;
        speakers[speaker].Process(planar[SpeakerChannel[speaker]].data(), frames, out, placement,
                                  sample_rate);
    }

    // The subwoofer channel has no direction; only what is deep enough to be one is kept.
    const float smoothing = 1.0f - std::exp(-2.0f * Pi * LfeCutoff / sample_rate);
    const float* lfe = planar[LfeChannel].data();
    float bass = bass_state;
    for (u32 i = 0; i < frames; ++i) {
        bass += (lfe[i] - bass) * smoothing;
        out[size_t(i) * 2] += bass * LfeLevel;
        out[size_t(i) * 2 + 1] += bass * LfeLevel;
    }
    bass_state = std::isfinite(bass) ? bass : 0.0f;

    // What still exceeds full scale is turned down as a whole rather than clipped.
    float gain = limiter_gain;
    for (u32 i = 0; i < frames; ++i) {
        const float peak =
            std::max(std::abs(out[size_t(i) * 2]), std::abs(out[size_t(i) * 2 + 1]));
        const float target = peak > 1.0f ? 1.0f / peak : 1.0f;
        gain = target < gain ? target : gain + (target - gain) * 0.0002f;
        out[size_t(i) * 2] *= gain;
        out[size_t(i) * 2 + 1] *= gain;
    }
    limiter_gain = std::isfinite(gain) ? gain : 1.0f;
}

bool VirtualSurroundEnabled() {
    int value = mode.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        value = 0;
        if (const char* env = std::getenv("SHADPS4_VIRTUAL_SURROUND"); env && *env) {
            value = *env != '0';
        }
#ifdef __ANDROID__
        char property[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.shadps4.virtual_surround", property) > 0) {
            value = property[0] != '0';
        }
#endif
        int expected = -1;
        mode.compare_exchange_strong(expected, value, std::memory_order_relaxed);
        value = mode.load(std::memory_order_relaxed);
    }
    return value != 0;
}

std::string VirtualSurroundCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        mode.store(args[0] == "on" ? 1 : 0);
        return fmt::format("audio_virtual_surround={}\n", args[0]);
    }
    if (args.empty() || (args.size() == 1 && args[0] == "status")) {
        return fmt::format("audio_virtual_surround={} (8-channel ports on a stereo output)\n",
                           VirtualSurroundEnabled() ? "on" : "off");
    }
    return "usage: audio_virtual_surround on | off | status\n";
}

} // namespace Libraries::AudioOut
