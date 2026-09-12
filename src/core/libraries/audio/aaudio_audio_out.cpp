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
// AAudio and convert guest S16 up-front; guest float is copied through.

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <aaudio/AAudio.h>

#include "common/logging/log.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"

namespace Libraries::AudioOut {

namespace {

constexpr float kInvVolume0dB = 1.0f / 32768.0f; // S16 -> [-1,1]
// Blocking-write timeout: generous relative to a buffer period; a write that
// cannot make progress within this returns and is logged rather than wedging the
// guest audio thread forever.
constexpr int64_t kWriteTimeoutNs = 200'000'000; // 200 ms

} // namespace

class AAudioPortBackend final : public PortBackend {
public:
    explicit AAudioPortBackend(const PortOut& port)
        : buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          num_channels(port.format_info.num_channels), is_float(port.format_info.is_float) {
        // AAudio output is always float here; size the conversion scratch to one
        // guest buffer of interleaved float. Allocated once, reused every Output.
        scratch.resize(static_cast<size_t>(buffer_frames) * num_channels);
        if (!OpenStream()) {
            LOG_ERROR(Lib_AudioOut, "Failed to open AAudio stream");
        }
    }

    ~AAudioPortBackend() override {
        CloseStream();
    }

    void Output(void* ptr) override {
        if (stream == nullptr || ptr == nullptr) [[unlikely]] {
            return;
        }
        const float* out = ToFloat(ptr);

        aaudio_result_t written =
            AAudioStream_write(stream, out, static_cast<int32_t>(buffer_frames), kWriteTimeoutNs);
        if (written == AAUDIO_ERROR_DISCONNECTED || written == AAUDIO_ERROR_INVALID_STATE) {
            // Device changed / stream died: rebuild once and retry this buffer.
            LOG_WARNING(Lib_AudioOut, "AAudio stream disconnected ({}); rebuilding",
                        AAudio_convertResultToText(written));
            CloseStream();
            if (OpenStream()) {
                written = AAudioStream_write(stream, out, static_cast<int32_t>(buffer_frames),
                                             kWriteTimeoutNs);
            }
        }
        if (written < 0) [[unlikely]] {
            LOG_ERROR(Lib_AudioOut, "AAudio write failed: {}",
                      AAudio_convertResultToText(written));
        }
    }

    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        // AAudio has no per-stream gain; the guest's PCM already carries level and
        // shadPS4's mixer applies volume upstream. Track the max for parity with
        // the other backends' semantics without a second gain stage.
        int max_v = 0;
        const u32 n = std::min<u32>(num_channels, 8);
        for (u32 i = 0; i < n; ++i) {
            max_v = std::max(max_v, ch_volumes[i]);
        }
        (void)max_v;
    }

private:
    const float* ToFloat(void* ptr) {
        if (is_float) {
            return static_cast<const float*>(ptr);
        }
        // S16 interleaved -> float, matching the SDL backend's ConvertS16*.
        const s16* s = static_cast<const s16*>(ptr);
        const size_t samples = static_cast<size_t>(buffer_frames) * num_channels;
        for (size_t i = 0; i < samples; ++i) {
            scratch[i] = static_cast<float>(s[i]) * kInvVolume0dB;
        }
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
        AAudioStreamBuilder_setBufferCapacityInFrames(
            builder, static_cast<int32_t>(buffer_frames) * 4);

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
        if (dev_channels != static_cast<int32_t>(num_channels)) {
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

    AAudioStream* stream{nullptr};
    std::vector<float> scratch;
};

std::unique_ptr<PortBackend> AAudioOut::Open(PortOut& port) {
    return std::make_unique<AAudioPortBackend>(port);
}

} // namespace Libraries::AudioOut
