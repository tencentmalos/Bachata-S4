// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// From AstroQuest (references/AstroQuest, src/core/libraries/audio/surround_virtualizer.*),
// reading guest PCM the way PrepareAudioStereo does.

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio3d/audio3d_spatializer.h"

namespace Libraries::AudioOut {

/// Turns 7.1 speaker audio into two channels for headphones (or the speakers of a headset, which
/// sit next to the ears) by placing seven virtual speakers around the listener, instead of
/// folding the channels down to left and right. One per 8-channel port: it keeps filter state
/// from block to block, so the blocks of a port must go through it in order.
class SurroundVirtualizer {
public:
    explicit SurroundVirtualizer(u32 max_frames, u32 sample_rate = 48000);

    /// Renders `frames` frames of 8-channel guest PCM into interleaved stereo, with the format,
    /// channel layout and gains PrepareAudioStereo takes. Does not allocate for up to
    /// `max_frames` frames.
    void Process(const AudioFormatInfo& info, u32 frames, const void* input,
                 const std::array<int, 8>& volume, float slider, std::span<float> output) noexcept;

private:
    static constexpr u32 NumSpeakers = 7;

    float sample_rate;
    std::array<Audio3d::Spatializer, NumSpeakers> speakers;
    std::array<std::vector<float>, 8> planar;
    float bass_state{};
    float limiter_gain{1.0f};
};

/// Whether 8-channel ports going to a stereo output are virtualized (default off; DebugBus
/// `audio_virtual_surround on|off`, SHADPS4_VIRTUAL_SURROUND / debug.shadps4.virtual_surround).
bool VirtualSurroundEnabled();
std::string VirtualSurroundCommand(const std::vector<std::string>& args);

} // namespace Libraries::AudioOut
