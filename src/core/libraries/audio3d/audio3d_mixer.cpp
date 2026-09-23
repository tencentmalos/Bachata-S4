// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_error.h"
#include "core/libraries/error_codes.h"
namespace Libraries::Audio3d {
static constexpr u32 AUDIO3D_OUTPUT_NUM_CHANNELS = 2;

static constexpr float DOWNMIX_FRONT = 1.0f;
static constexpr float DOWNMIX_CENTER = 0.7071f;
static constexpr float DOWNMIX_SURROUND = 0.7071f;
static constexpr float DOWNMIX_LFE = 0.0f;

u32 ProcessMixQueue(Port& port) {
    const u32 granularity = port.parameters.granularity;
    const u32 out_samples = granularity * AUDIO3D_OUTPUT_NUM_CHANNELS;

    // ---- FLOAT MIX BUFFER ----
    float* mix_float = static_cast<float*>(std::calloc(out_samples, sizeof(float)));

    if (!mix_float)
        return ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY;

    s16* mix_s16 = static_cast<s16*>(std::malloc(out_samples * sizeof(s16)));
    if (!mix_s16) {
        std::free(mix_float);
        return ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY;
    }
    try {
        port.mixed_queue.push_back(AudioData{reinterpret_cast<u8*>(mix_s16), granularity,
                                             AUDIO3D_OUTPUT_NUM_CHANNELS,
                                             OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16});
    } catch (...) {
        std::free(mix_float);
        std::free(mix_s16);
        throw;
    }
    auto mix_in = [&](std::deque<AudioData>& queue, const float gain, u32 passthrough = 0) {
        if (queue.empty())
            return;

        // default gain is 0.0 — objects with no GAIN set are silent.
        if (gain == 0.0f) {
            AudioData data = queue.front();
            queue.pop_front();
            std::free(data.sample_buffer);
            return;
        }

        AudioData data = queue.front();
        queue.pop_front();

        const u32 frames = std::min(granularity, data.num_samples);
        const u32 channels = data.num_channels;

        if (data.format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16) {
            const s16* src = reinterpret_cast<const s16*>(data.sample_buffer);

            for (u32 i = 0; i < frames; i++) {
                float left = 0.0f;
                float right = 0.0f;

                if (channels == 1) {
                    float v = src[i] / 32768.0f;
                    left = v;
                    right = v;
                } else {
                    const auto sample = [&](const u32 c) {
                        return src[i * channels + c] / 32768.0f;
                    };
                    left = DOWNMIX_FRONT * sample(0);
                    right = DOWNMIX_FRONT * sample(1);
                    if (channels >= 3) {
                        const float center = DOWNMIX_CENTER * sample(2);
                        left += center;
                        right += center;
                    }
                    if constexpr (DOWNMIX_LFE != 0.0f) {
                        if (channels >= 4) {
                            const float lfe = DOWNMIX_LFE * sample(3);
                            left += lfe;
                            right += lfe;
                        }
                    }
                    if (channels >= 6) {
                        left += DOWNMIX_SURROUND * sample(4);
                        right += DOWNMIX_SURROUND * sample(5);
                    }
                    if (channels >= 8) {
                        left += DOWNMIX_SURROUND * sample(6);
                        right += DOWNMIX_SURROUND * sample(7);
                    }
                }

                mix_float[i * 2 + 0] += left * gain * (passthrough == 2 ? 0.0f : 1.0f);
                mix_float[i * 2 + 1] += right * gain * (passthrough == 1 ? 0.0f : 1.0f);
            }
        } else { // FLOAT input
            const float* src = reinterpret_cast<const float*>(data.sample_buffer);

            for (u32 i = 0; i < frames; i++) {
                float left = 0.0f;
                float right = 0.0f;

                if (channels == 1) {
                    left = src[i];
                    right = src[i];
                } else {
                    // Same multichannel fold as the S16 branch above.
                    const auto sample = [&](const u32 c) { return src[i * channels + c]; };
                    left = DOWNMIX_FRONT * sample(0);
                    right = DOWNMIX_FRONT * sample(1);
                    if (channels >= 3) {
                        const float center = DOWNMIX_CENTER * sample(2);
                        left += center;
                        right += center;
                    }
                    if constexpr (DOWNMIX_LFE != 0.0f) {
                        if (channels >= 4) {
                            const float lfe = DOWNMIX_LFE * sample(3);
                            left += lfe;
                            right += lfe;
                        }
                    }
                    if (channels >= 6) {
                        left += DOWNMIX_SURROUND * sample(4);
                        right += DOWNMIX_SURROUND * sample(5);
                    }
                    if (channels >= 8) {
                        left += DOWNMIX_SURROUND * sample(6);
                        right += DOWNMIX_SURROUND * sample(7);
                    }
                }

                mix_float[i * 2 + 0] += left * gain * (passthrough == 2 ? 0.0f : 1.0f);
                mix_float[i * 2 + 1] += right * gain * (passthrough == 1 ? 0.0f : 1.0f);
            }
        }

        std::free(data.sample_buffer);
    };

    mix_in(port.bed_queue, 1.0f);

    for (auto& [obj_id, obj] : port.objects) {
        float gain = 0.0f;
        const auto gain_key =
            static_cast<u32>(OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_GAIN);
        if (obj.persistent_attributes.contains(gain_key)) {
            const auto& blob = obj.persistent_attributes.at(gain_key);
            if (blob.size() >= sizeof(float)) {
                std::memcpy(&gain, blob.data(), sizeof(float));
            }
        }
        u32 passthrough{};
        const auto it = obj.persistent_attributes.find(6);
        if (it != obj.persistent_attributes.end() && it->second.size() >= 4)
            std::memcpy(&passthrough, it->second.data(), 4);
        mix_in(obj.pcm_queue, gain, passthrough);
    }

    for (u32 i = 0; i < out_samples; i++) {
        float v = std::isfinite(mix_float[i]) ? std::clamp(mix_float[i], -1.0f, 1.0f) : 0.0f;
        mix_s16[i] = static_cast<s16>(v * 32767.0f);
    }

    std::free(mix_float);

    std::erase_if(port.objects, [](const auto& kv) {
        return kv.second.unreserved && kv.second.pcm_queue.empty();
    });

    return ORBIS_OK;
}

} // namespace Libraries::Audio3d
