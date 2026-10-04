// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <limits>
#include <gtest/gtest.h>

#include "core/libraries/audio/audioout_transfer.h"

using namespace Libraries::AudioOut;

namespace {

// Same entries as GetFormatInfo in audioout.cpp.
constexpr AudioFormatInfo Float8 = {true, 4, 8, {0, 1, 2, 3, 4, 5, 6, 7}, false};
constexpr AudioFormatInfo Float8Std = {true, 4, 8, {0, 1, 2, 3, 6, 7, 4, 5}, true};
constexpr AudioFormatInfo S16_8 = {false, 2, 8, {0, 1, 2, 3, 4, 5, 6, 7}, false};
constexpr std::array<int, 8> FullVolume = {32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768};

std::array<float, 8> Surround(const AudioFormatInfo& format, const void* input,
                              const std::array<int, 8>& volume = FullVolume, float slider = 1) {
    std::array<float, 8> out{};
    PrepareAudioSurround71(format, 1, input, volume, slider, out);
    return out;
}

} // namespace

// PS4 8ch order is L R C LFE Ls Rs Le Re; the device gets FL FR FC LFE BL BR SL SR.
TEST(AudioTransfer, Surround71MapsGuestChannelsToWaveOrder) {
    constexpr std::array<int, 8> wave_index_of_guest = {0, 1, 2, 3, 6, 7, 4, 5};
    for (int ch = 0; ch < 8; ++ch) {
        std::array<float, 8> impulse{};
        impulse[ch] = 0.5f;
        const auto out = Surround(Float8, impulse.data());
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(out[i], i == wave_index_of_guest[ch] ? 0.5f : 0.0f) << ch << " -> " << i;
        }
    }
}

// The Std formats already carry the WAVE order.
TEST(AudioTransfer, Surround71KeepsStdOrder) {
    for (int ch = 0; ch < 8; ++ch) {
        std::array<float, 8> impulse{};
        impulse[ch] = 0.25f;
        const auto out = Surround(Float8Std, impulse.data());
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(out[i], i == ch ? 0.25f : 0.0f) << ch << " -> " << i;
        }
    }
}

// Volume flags index the guest logical channels (L R C LFE LS RS LE RE).
TEST(AudioTransfer, Surround71AppliesPerChannelVolume) {
    std::array<float, 8> ones;
    ones.fill(1.0f);
    std::array<int, 8> volume = FullVolume;
    volume[4] = 16384; // LS
    volume[6] = 0;     // LE
    const auto out = Surround(Float8, ones.data(), volume, 0.5f);
    EXPECT_EQ(out[0], 0.5f);
    EXPECT_EQ(out[6], 0.25f); // SL comes from LS
    EXPECT_EQ(out[4], 0.0f);  // BL comes from LE
}

TEST(AudioTransfer, Surround71ConvertsS16AndDropsNonFinite) {
    std::array<s16, 8> pcm{16384, -32768, 0, 0, 0, 0, 0, 8192};
    const auto out = Surround(S16_8, pcm.data());
    EXPECT_EQ(out[0], 0.5f);
    EXPECT_EQ(out[1], -1.0f);
    EXPECT_EQ(out[5], 0.25f); // BR comes from RE

    std::array<float, 8> bad{};
    bad[2] = std::numeric_limits<float>::quiet_NaN();
    bad[3] = std::numeric_limits<float>::infinity();
    const auto clean = Surround(Float8, bad.data());
    EXPECT_EQ(clean[2], 0.0f);
    EXPECT_EQ(clean[3], 0.0f);
}

// The stereo fold used by both the Android Oboe and the desktop cubeb backends.
TEST(AudioTransfer, StereoFoldKeepsCenterAndSurrounds) {
    for (int ch = 0; ch < 8; ++ch) {
        std::array<float, 8> impulse{};
        impulse[ch] = 1.0f;
        std::array<float, 2> stereo{};
        PrepareAudioStereo(Float8, 1, impulse.data(), FullVolume, 1, stereo);
        const float left = ch == 0 ? 1 : ch == 2 || ch == 4 || ch == 6 ? 0.7071f : 0;
        const float right = ch == 1 ? 1 : ch == 2 || ch == 5 || ch == 7 ? 0.7071f : 0;
        EXPECT_NEAR(stereo[0], left, 1e-6f) << ch;
        EXPECT_NEAR(stereo[1], right, 1e-6f) << ch;
    }
}
