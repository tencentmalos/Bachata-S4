// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/hmd/hmd.h"
#include "video_core/vr_frame.h"

namespace Core::HostRuntime::HmdDiagnostics {
using Json = nlohmann::json;
using Args = std::array<u64, 6>;

// Per entry, per session: at most 28 samples, including initialization and
// later steady-state calls. No unbounded per-frame logging or guest writes.
inline bool Sample(u64 call) {
    return call && (call <= 16 || (call <= 65536 && (call & (call - 1)) == 0));
}

inline std::string Hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto value = std::to_integer<u8>(bytes[i]);
        out[2 * i] = digits[value >> 4];
        out[2 * i + 1] = digits[value & 15];
    }
    return out;
}
template <class T> inline Json Bytes(const T& value) {
    return Hex(std::as_bytes(std::span{&value, 1}));
}

template <class T>
bool Read(GuestCpu::GuestAddressSpace& space, Json& records, std::string name,
          u64 address, T& value) {
    Json item{{"field", std::move(name)}, {"address", address}, {"size", sizeof(T)}};
    // Acquire through the VM's normal generation/permission checks. Do not
    // dereference guest addresses, retain leases, or manufacture zero outputs.
    const bool ok = address && bool(space.ReadData(GuestCpu::GuestAddress{address},
                                    std::as_writable_bytes(std::span{&value, 1})));
    item["readable"] = ok;
    if (ok) item["le_hex"] = Bytes(value);
    records.push_back(std::move(item));
    return ok;
}

inline void EyePair(GuestCpu::GuestAddressSpace& space, Json& records,
                    std::string prefix, const u64* textures, u64 sampler) {
    AmdGpu::Image image{};
    AmdGpu::Sampler state{};
    for (unsigned i = 0; i < 2; ++i)
        Read(space, records, prefix + ".texture" + std::to_string(i), textures[i], image);
    Read(space, records, prefix + ".sampler", sampler, state);
}

// Snapshot the call boundary, including the complete unknown wire fields.
// Decoding must not erase reserved fields, padding, NaN payloads or flags.
inline Json Records(GuestCpu::GuestAddressSpace& space, std::string_view name,
                    const Args& a, bool output, u64 result = 0) {
    using namespace Libraries::Hmd;
    Json records = Json::array();
    if (output) {
        if (result) return records; // failed APIs do not promise output data
        if (name == "sceHmdGetFieldOfView") {
            OrbisHmdFieldOfView f{};
            if (Read(space, records, "fov", a[1], f))
                records.back()["tan_out_in_top_bottom"] = {f.tan_out, f.tan_in, f.tan_top, f.tan_bottom};
        } else if (name == "sceHmdGetDeviceInformation" ||
                   name == "sceHmdGetDeviceInformationByHandle") {
            OrbisHmdDeviceInformation d{};
            if (Read(space, records, "device", a[name.ends_with("ByHandle") ? 1 : 0], d)) {
                records.back()["panel"] = {d.device_info.panel_resolution.width, d.device_info.panel_resolution.height};
                records.back()["status"] = u32(d.status);
                records.back()["user"] = d.user_id;
                records.back()["mounted"] = d.hmu_mount;
            }
        } else if (name == "sceHmdGet2DEyeOffset") {
            // Firmware writes 12 bytes, leaving the remaining 20 caller-owned.
            std::array<float, 3> offset{};
            for (unsigned i = 0; i < 2; ++i)
                if (Read(space, records, "eye_offset" + std::to_string(i), a[i + 1], offset))
                    records.back()["xyz"] = offset;
        }
        return records;
    }
    if (name == "sceHmdInitialize" || name == "sceHmdInitialize315") {
        std::array<u8, 16> p{}; Read(space, records, "initialize", a[0], p);
    } else if (name == "sceHmdReprojectionInitialize") {
        OrbisHmdReprojectionInitializeParam p{}; Read(space, records, "initialize", a[0], p);
    } else if (name == "sceHmdReprojectionStartMultilayer") {
        OrbisHmdReprojectionSubmission submit{};
        std::array<u8, 56> pose{};
        Read(space, records, "submission", a[2], submit);
        Read(space, records, "pose", a[3], pose);
        for (u64 i = 0; i < std::min<u64>(a[1], 3); ++i) {
            OrbisHmdReprojectionLayer layer{};
            if (a[0] > UINT64_MAX - i * sizeof(layer)) break;
            const auto prefix = "layer" + std::to_string(i);
            if (!Read(space, records, prefix, a[0] + i * sizeof(layer), layer)) continue;
            std::array<float, 16> transforms{};
            std::memcpy(transforms.data(), layer.color_tan_to_uv, 32);
            std::memcpy(transforms.data() + 8, layer.opaque48, 32);
            records.back()["float28_64"] = transforms;
            records.back()["float68_6c"] = {layer.field68, layer.field6c};
            records.back()["kind78"] = layer.kind78;
            EyePair(space, records, prefix, layer.roots00, layer.root20);
            if (layer.optional70) {
                std::array<u8, 136> warp{};
                Read(space, records, prefix + ".optional70", layer.optional70, warp);
            }
        }
    } else if (name == "sceHmdReprojectionStart" || name == "sceHmdReprojectionStartWithOverlay") {
        OrbisHmdReprojectionStartParam p{};
        if (Read(space, records, "base", a[0], p)) {
            records.back()["tan_to_uv"] = p.tan_to_uv;
            EyePair(space, records, "base", p.textures, p.sampler);
        }
        std::array<u8, 56> pose{}; Read(space, records, "pose", a[1], pose);
        if (name == "sceHmdReprojectionStartWithOverlay") {
            OrbisHmdReprojectionOverlayParam o{};
            if (Read(space, records, "overlay", a[3], o)) {
                records.back()["tan_to_uv"] = o.tan_to_uv;
                EyePair(space, records, "overlay", o.textures, o.sampler);
            }
        }
    } else if (name == "sceHmdReprojectionStart2dVr") {
        OrbisHmdReprojection2dParam p{};
        if (Read(space, records, "2d", a[0], p)) {
            const u64 eyes[]{p.texture, p.texture};
            EyePair(space, records, "2d", eyes, p.sampler);
        }
    }
    return records;
}

inline Json Frame(const VideoCore::VrFrameSource& frame) {
    Json result = Json::array();
    for (unsigned i = 0; i < std::min<unsigned>(frame.image_count, frame.eyes.size()); ++i) {
        const auto& e = frame.eyes[i];
        result.push_back({{"eye", i}, {"texture_le_hex", Bytes(e)},
            {"sampler_le_hex", Bytes(frame.samplers[i])}, {"uv_scale_bias", frame.uv[i]},
            {"width", u32(e.width) + 1}, {"height", u32(e.height) + 1}, {"pitch", e.Pitch()},
            {"base_array", u32(e.base_array)}, {"last_array", u32(e.last_array)}});
    }
    return result;
}
} // namespace Core::HostRuntime::HmdDiagnostics
