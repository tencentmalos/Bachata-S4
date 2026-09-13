// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Android AAudio output backend.
//
// Mirrors the SDL/OpenAL PortBackend contract: PortBackend::Output(void*) is a
// guest-driven blocking push, called at ~buffer-period intervals with one full
// port buffer. Rather than a data-callback + ring buffer (citron's Oboe model),
// this uses AAudio's blocking write (AAudioStream_write with a timeout): the guest
// thread already paces the calls, and AAudio's own device buffer provides the
// decoupling, so there is no audio-callback thread to keep RT-safe -- no locks,
// no allocation, no JNI, no guest re-entry on any callback, because there is no
// callback. Disconnect (AAUDIO_ERROR_DISCONNECTED) rebuilds the stream, the same
// recovery citron gets from Oboe's onErrorAfterClose.
//
// Reference: citron oboe_sink.cpp ConfigureBuilder (builder settings) and the SDL
// backend's Convert* family (S16/F32 interleaved conversion). We output float to
// AAudio and convert guest S16 up-front; guest float and S16 both receive channel mapping and
// actual gain.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include <aaudio/AAudio.h>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/audio/audioout_transfer.h"

namespace Libraries::AudioOut {

class AAudioPortBackend final : public PortBackend {
public:
    explicit AAudioPortBackend(const PortOut& port)
        : buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          num_channels(port.format_info.num_channels), is_float(port.format_info.is_float),
          format_info(port.format_info) {
        // AAudio output is always float here; size the conversion scratch to one
        // guest buffer of interleaved float. Allocated once, reused every Output.
        scratch.resize(static_cast<size_t>(buffer_frames) * num_channels);
        if (!OpenStream()) {
            throw std::runtime_error("Failed to open AAudio stream");
        }
    }

    ~AAudioPortBackend() override {
        CloseStream();
    }

    void Output(void* ptr) override {
        const int error = OutputChecked(ptr, {});
        if (error < 0)
            LOG_ERROR(Lib_AudioOut, "AAudio output failed: {}", AAudio_convertResultToText(error));
    }

    int OutputChecked(void* ptr, std::stop_token stop) override {
        if (!stream || !ptr)
            return AAUDIO_ERROR_INVALID_STATE;
        const float* out = ToFloat(ptr);
        const auto result = TransferAudioFrames(
            buffer_frames, stop,
            [&](u32 offset, u32 remaining, int64_t timeout) {
                return AAudioStream_write(stream, out + offset * num_channels, remaining, timeout);
            },
            [&](int error) {
                if (error != AAUDIO_ERROR_DISCONNECTED && error != AAUDIO_ERROR_INVALID_STATE)
                    return false;
                CloseStream();
                return !stop.stop_requested() && OpenStream();
            });
        if (result.cancelled)
            return AAUDIO_ERROR_INVALID_STATE;
        if (result.timed_out)
            return AAUDIO_ERROR_TIMEOUT;
        return result.error;
    }

    void SetVolume(const std::array<int, 8>& volumes) override {
        channel_volumes = volumes;
    }

private:
    const float* ToFloat(void* ptr) {
        ConvertAudioFrames(format_info, buffer_frames, ptr, channel_volumes,
                           EmulatorSettings.GetVolumeSlider() * 0.01f, scratch);
        return scratch.data();
    }

    bool OpenStream() {
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t r = AAudio_createStreamBuilder(&builder);
        if (r != AAUDIO_OK || builder == nullptr) {
            LOG_ERROR(Lib_AudioOut, "AAudio_createStreamBuilder failed: {}",
                      AAudio_convertResultToText(r));
            return false;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sample_rate));
        AAudioStreamBuilder_setChannelCount(builder, static_cast<int32_t>(num_channels));
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        // Blocking-write model: no data callback. AAudio's own buffer decouples us
        // from the device; keep a couple of guest buffers of capacity.
        AAudioStreamBuilder_setBufferCapacityInFrames(builder,
                                                      static_cast<int32_t>(buffer_frames) * 4);

        r = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (r != AAUDIO_OK || stream == nullptr) {
            LOG_ERROR(Lib_AudioOut, "AAudioStreamBuilder_openStream failed: {}",
                      AAudio_convertResultToText(r));
            stream = nullptr;
            return false;
        }

        // The device may not honor the requested channel count; if it changed,
        // our interleaving no longer matches. Report and bail rather than emit
        // garbled audio.
        const int32_t dev_channels = AAudioStream_getChannelCount(stream);
        if (dev_channels != static_cast<int32_t>(num_channels) ||
            AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_FLOAT ||
            AAudioStream_getSampleRate(stream) != static_cast<int32_t>(sample_rate)) {
            LOG_ERROR(Lib_AudioOut, "AAudio granted {} channels, requested {}", dev_channels,
                      num_channels);
            CloseStream();
            return false;
        }

        r = AAudioStream_requestStart(stream);
        if (r != AAUDIO_OK) {
            LOG_ERROR(Lib_AudioOut, "AAudioStream_requestStart failed: {}",
                      AAudio_convertResultToText(r));
            CloseStream();
            return false;
        }

        LOG_INFO(Lib_AudioOut, "Opened AAudio output: {} Hz, {} ch, {} (perf={})", sample_rate,
                 num_channels, is_float ? "float" : "s16->float",
                 static_cast<int>(AAudioStream_getPerformanceMode(stream)));
        return true;
    }

    void CloseStream() {
        if (stream != nullptr) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            stream = nullptr;
        }
    }

    const u32 buffer_frames;
    const u32 sample_rate;
    const u32 num_channels;
    const bool is_float;
    const AudioFormatInfo format_info;
    std::array<int, 8> channel_volumes{32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768};

    AAudioStream* stream{nullptr};
    std::vector<float> scratch;
};

std::unique_ptr<PortBackend> AAudioOut::Open(PortOut& port) {
    return std::make_unique<AAudioPortBackend>(port);
}

} // namespace Libraries::AudioOut
