// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_error.h"
#include "core/libraries/error_codes.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
namespace Libraries::Audio3d {
static constexpr u32 AUDIO3D_OUTPUT_NUM_CHANNELS = 2;
static constexpr float AUDIO3D_MIX_RATE = 48000.0f;

namespace {

// -1 until read from SHADPS4_AUDIO3D_SPATIAL / debug.shadps4.audio3d_spatial (0|1). Off by
// default until a title with positioned objects shows which way its +Z points (front_z): TMNT
// renders no objects, MHW's title, menus and opening only its bed.
std::atomic<int> spatial_mode{-1};
// +1: the title's +Z points ahead of the listener (the OpenAL path assumes so); -1: behind.
std::atomic<int> front_z{1};

// Where the sound energy of positioned objects came from, summed per mixed block.
struct SpatialStats {
    std::atomic<u64> blocks{}, positioned{}, unpositioned{};
    std::atomic<double> energy{}, energy_ahead{}, energy_behind{}, energy_left{}, energy_right{},
        weighted_distance{};
};
SpatialStats stats;

void Add(std::atomic<double>& value, double amount) {
    double current = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(current, current + amount, std::memory_order_relaxed)) {
    }
}

bool SpatialEnabled() {
    int mode = spatial_mode.load(std::memory_order_relaxed);
    if (mode < 0) [[unlikely]] {
        mode = 0;
        if (const char* env = std::getenv("SHADPS4_AUDIO3D_SPATIAL"); env && *env) {
            mode = *env != '0';
        }
#ifdef __ANDROID__
        char property[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.shadps4.audio3d_spatial", property) > 0) {
            mode = property[0] != '0';
        }
#endif
        int expected = -1;
        spatial_mode.compare_exchange_strong(expected, mode, std::memory_order_relaxed);
        mode = spatial_mode.load(std::memory_order_relaxed);
    }
    return mode != 0;
}

/// The object's position and spread, when the title has given a position.
bool ReadPlacement(const ObjectState& obj, float gain, ObjectPlacement& placement) {
    const auto pos =
        obj.persistent_attributes.find(u32(OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_POSITION));
    if (pos == obj.persistent_attributes.end() || pos->second.size() < 3 * sizeof(float)) {
        return false;
    }
    float xyz[3];
    std::memcpy(xyz, pos->second.data(), sizeof(xyz));
    placement.x = xyz[0];
    placement.y = xyz[1];
    placement.z = front_z.load(std::memory_order_relaxed) > 0 ? -xyz[2] : xyz[2];
    placement.gain = gain;
    placement.spread = 0.0f;
    const auto spread =
        obj.persistent_attributes.find(u32(OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_SPREAD));
    if (spread != obj.persistent_attributes.end() && spread->second.size() >= sizeof(float)) {
        std::memcpy(&placement.spread, spread->second.data(), sizeof(float));
    }
    return true;
}

} // namespace

std::string SpatialCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        spatial_mode.store(args[0] == "on" ? 1 : 0);
        return fmt::format("audio3d_spatial={}\n", args[0]);
    }
    if (args.size() == 2 && args[0] == "front_z" && (args[1] == "+" || args[1] == "-")) {
        front_z.store(args[1] == "+" ? 1 : -1);
        return fmt::format("title +Z is {} the listener\n", args[1] == "+" ? "ahead of" : "behind");
    }
    if (args.empty() || (args.size() == 1 && args[0] == "status")) {
        const double energy = stats.energy.load();
        const auto share = [&](const std::atomic<double>& part) {
            return energy > 0 ? part.load() / energy : 0.0;
        };
        return fmt::format(
            "audio3d_spatial={} front_z={}\n"
            "blocks={} positioned_object_blocks={} unpositioned_object_blocks={}\n"
            "energy share by title axes: +Z {:.3f} -Z {:.3f} +X {:.3f} -X {:.3f} "
            "(weighted distance {:.2f} m)\n",
            SpatialEnabled() ? "on" : "off", front_z.load() > 0 ? "+" : "-", stats.blocks.load(),
            stats.positioned.load(), stats.unpositioned.load(), share(stats.energy_ahead),
            share(stats.energy_behind), share(stats.energy_right), share(stats.energy_left),
            energy > 0 ? stats.weighted_distance.load() / energy : 0.0);
    }
    if (args.size() == 1 && args[0] == "reset") {
        stats.blocks = 0;
        stats.positioned = 0;
        stats.unpositioned = 0;
        stats.energy = 0;
        stats.energy_ahead = 0;
        stats.energy_behind = 0;
        stats.energy_left = 0;
        stats.energy_right = 0;
        stats.weighted_distance = 0;
        return "audio3d_spatial statistics reset\n";
    }
    return "usage: audio3d_spatial on | off | front_z +|- | status | reset\n";
}

static constexpr float DOWNMIX_FRONT = 1.0f;
static constexpr float DOWNMIX_CENTER = 0.7071f;
static constexpr float DOWNMIX_SURROUND = 0.7071f;
static constexpr float DOWNMIX_LFE = 0.0f;

u32 ProcessMixQueue(Port& port) {
    const u32 granularity = port.parameters.granularity;
    const u32 out_samples = granularity * AUDIO3D_OUTPUT_NUM_CHANNELS;

    // ---- FLOAT MIX BUFFER ----
    port.mix_scratch.assign(out_samples, 0.0f);
    float* mix_float = port.mix_scratch.data();

    s16* mix_s16 = static_cast<s16*>(std::malloc(out_samples * sizeof(s16)));
    if (!mix_s16) {
        return ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY;
    }
    try {
        port.mixed_queue.push_back(AudioData{reinterpret_cast<u8*>(mix_s16), granularity,
                                             AUDIO3D_OUTPUT_NUM_CHANNELS,
                                             OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16});
    } catch (...) {
        std::free(mix_s16);
        throw;
    }
    const bool spatial = SpatialEnabled();
    stats.blocks.fetch_add(1, std::memory_order_relaxed);

    // A positioned mono object goes through the head model; the rest is mixed as before.
    auto spatialize = [&](ObjectState& obj, const ObjectPlacement& placement) {
        AudioData data = obj.pcm_queue.front();
        obj.pcm_queue.pop_front();
        const u32 frames = std::min(granularity, data.num_samples);
        port.mono_scratch.resize(granularity);
        float* mono = port.mono_scratch.data();
        if (data.format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16) {
            const s16* src = reinterpret_cast<const s16*>(data.sample_buffer);
            for (u32 i = 0; i < frames; i++)
                mono[i] = src[i] / 32768.0f;
        } else {
            std::memcpy(mono, data.sample_buffer, frames * sizeof(float));
        }
        std::fill(mono + frames, mono + granularity, 0.0f);
        obj.spatializer.Process(mono, granularity, mix_float, placement, AUDIO3D_MIX_RATE);
        std::free(data.sample_buffer);
    };
    auto record = [&](const ObjectState& obj, const ObjectPlacement* placement, float gain) {
        if (!placement) {
            stats.unpositioned.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        stats.positioned.fetch_add(1, std::memory_order_relaxed);
        const AudioData& data = obj.pcm_queue.front();
        if (data.num_channels != 1)
            return;
        const u32 frames = std::min(granularity, data.num_samples);
        double energy = 0.0;
        if (data.format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16) {
            const s16* src = reinterpret_cast<const s16*>(data.sample_buffer);
            for (u32 i = 0; i < frames; i++)
                energy += double(src[i]) * src[i] * (1.0 / (32768.0 * 32768.0));
        } else {
            const float* src = reinterpret_cast<const float*>(data.sample_buffer);
            for (u32 i = 0; i < frames; i++)
                energy += double(src[i]) * src[i];
        }
        energy *= double(gain) * gain;
        const auto pos = obj.persistent_attributes.find(
            u32(OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_POSITION));
        float xyz[3];
        std::memcpy(xyz, pos->second.data(), sizeof(xyz));
        const double length = std::sqrt(double(xyz[0]) * xyz[0] + double(xyz[1]) * xyz[1] +
                                        double(xyz[2]) * xyz[2]);
        if (!(energy > 0) || !std::isfinite(energy) || !(length > 1e-4) || !std::isfinite(length))
            return;
        Add(stats.energy, energy);
        Add(xyz[2] > 0 ? stats.energy_ahead : stats.energy_behind, energy * std::abs(xyz[2]) / length);
        Add(xyz[0] > 0 ? stats.energy_right : stats.energy_left, energy * std::abs(xyz[0]) / length);
        Add(stats.weighted_distance, energy * length);
    };
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
        if (passthrough == 0 && !obj.pcm_queue.empty() && gain != 0.0f) {
            ObjectPlacement placement;
            const bool positioned = ReadPlacement(obj, gain, placement);
            record(obj, positioned ? &placement : nullptr, gain);
            if (spatial && positioned && obj.pcm_queue.front().num_channels == 1) {
                spatialize(obj, placement);
                continue;
            }
        }
        mix_in(obj.pcm_queue, gain, passthrough);
    }

    for (u32 i = 0; i < out_samples; i++) {
        float v = std::isfinite(mix_float[i]) ? std::clamp(mix_float[i], -1.0f, 1.0f) : 0.0f;
        mix_s16[i] = static_cast<s16>(v * 32767.0f);
    }

    std::erase_if(port.objects, [](const auto& kv) {
        return kv.second.unreserved && kv.second.pcm_queue.empty();
    });

    return ORBIS_OK;
}

} // namespace Libraries::Audio3d
