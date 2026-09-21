// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_data_batch.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "video_core/vr_frame.h"

namespace Core::HostRuntime {
// SBS transport only: no optical distortion or pose/time warp. Guest records
// are copied at admission; the renderer owns the GPU read leases until retirement.
struct ReprojectionFrame : VideoCore::VrFrameSource {
    u64 completion_label{}, sequence{};
    s32 video_handle{}, display_index{};
};
inline constexpr std::string_view ReprojectionNids[]{
    "OuygGEWkins", "ZrV5YIqD09I", "vzMEkwBQciM", "E+dPfjeQLHI", "iGNNpDDjcwo",
    "7as0CjXW1B8", "knyIhlkpLgE", "mdyFbaJj66M", "94+Ggm38KCg", "8gH1aLgty5I",
    "dntZTJ7meIU", "q3e8+nEguyE", "kcldQ7zLYQQ"};

class GuestReprojection final {
public:
    struct Backend {
        std::function<s32(s32, s32, s32)> validate_display;
        std::function<bool(s64, u32)> event_exists;
        std::function<void(s64, u32)> trigger;
        std::function<void(std::function<void()>)> set_cadence;
        std::function<void(bool)> set_active;
        std::function<s32(ReprojectionFrame, std::function<void(bool)>)> submit;
    };

private:
    struct Event {
        s64 queue{};
        u32 id{};
    };
    struct State {
        std::mutex mutex;
        std::condition_variable done;
        bool initialized{}, display{}, stopped{};
        s32 video{}, indices[2]{};
        u32 next{}, pending{};
        std::array<bool, 2> busy{};
        u64 epoch{}, submitted{}, completed{};
        Event start{}, end{};
    };
    GuestCpu::GuestAddressSpace& space;
    Backend backend;
    std::shared_ptr<State> state = std::make_shared<State>();
    template <typename T>
    bool Read(u64 address, T& value) {
        return address && bool(space.ReadData(GuestCpu::GuestAddress{address},
                                              std::as_writable_bytes(std::span{&value, 1})));
    }
    bool ValidateWorkAreas(u64 onion, u64 garlic) {
        using namespace GuestCpu;
        GuestDataBatch batch;
        return batch.Add(space, {{onion}, 0x810}, GuestPermission::Write) &&
               batch.Add(space, {{garlic}, 0x100000}, GuestPermission::Write) &&
               bool(batch.Acquire(space));
    }

    static bool ValidUv(const std::array<float, 4>& uv) {
        return std::ranges::all_of(uv, [](float v) { return std::isfinite(v); }) && uv[0] != 0.f &&
               uv[1] != 0.f;
    }
    // The submitted vectors map view-ray tangents to texture coordinates.
    // Project the same virtual frustum advertised by GetFieldOfView into each
    // source independently. This preserves atlas cropping and dynamic size.
    static std::array<float, 4> EyeUv(const float* transform, u32 eye) {
        const auto& f = Libraries::Hmd::SbsFieldOfView;
        const float left = eye ? f.tan_in : f.tan_out;
        return {transform[0] * (f.tan_out + f.tan_in), transform[1] * (f.tan_top + f.tan_bottom),
                transform[2] - transform[0] * left, transform[3] - transform[1] * f.tan_bottom};
    }
    u32 ReadEyePair(ReprojectionFrame& frame, const u64* textures, u64 sampler_address,
                    const float* transforms, bool tangent_space = true) {
        using namespace Libraries::Hmd;
        AmdGpu::Sampler sampler{};
        if (!Read(sampler_address, sampler))
            return ORBIS_HMD_ERROR_PARAMETER_NULL;
        if (sampler.force_unnormalized ||
            sampler.depth_compare_func != AmdGpu::DepthCompare::Never ||
            sampler.filter_mode != AmdGpu::FilterMode::Blend ||
            sampler.border_color_type == AmdGpu::BorderColor::Custom ||
            u32(sampler.mip_filter.Value()) > 2 || u32(sampler.max_aniso.Value()) > 4 ||
            sampler.min_lod > sampler.max_lod)
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        for (u32 eye = 0; eye < 2; ++eye) {
            const auto index = frame.image_count;
            if (!Read(textures[eye], frame.eyes[index]))
                return ORBIS_HMD_ERROR_PARAMETER_NULL;
            const auto* t = transforms + eye * 4;
            auto uv = tangent_space ? EyeUv(t, eye) : std::array<float, 4>{t[0], t[1], t[2], t[3]};
            if (!ValidUv(uv))
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            frame.samplers[index] = sampler;
            frame.uv[index] = uv;
            ++frame.image_count;
        }
        return 0;
    }
    u32 ReadLegacy(std::string_view nid, const std::array<u64, 6>& a, ReprojectionFrame& frame) {
        using namespace Libraries::Hmd;
        if (nid == "q3e8+nEguyE") {
            if (a[2])
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            OrbisHmdReprojection2dParam p{};
            if (!Read(a[0], p) || !p.release_label)
                return ORBIS_HMD_ERROR_PARAMETER_NULL;
            if (p.release_label & 7)
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            for (auto r : p.reserved)
                if (r)
                    return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            frame.completion_label = p.release_label;
            frame.sequence = a[1];
            const u64 textures[2]{p.texture, p.texture};
            const float uv[8]{p.uv[0], p.uv[1], p.uv[2], p.uv[3],
                              p.uv[0], p.uv[1], p.uv[2], p.uv[3]};
            return ReadEyePair(frame, textures, p.sampler, uv, false);
        }
        const bool overlay = nid == "kcldQ7zLYQQ";
        if (a[overlay ? 4 : 3])
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        OrbisHmdReprojectionStartParam p{};
        std::array<u8, 0x38> pose{};
        if (!Read(a[0], p) || !Read(a[1], pose) || !p.release_label)
            return ORBIS_HMD_ERROR_PARAMETER_NULL;
        if ((p.release_label & 7) || p.selector < 2000 || p.selector > 7000 || p.selector50 > 1 ||
            (p.flags & 0xffffffff0ffffff0ULL) || (p.selector < 3000 && (p.flags & 1)))
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        for (auto r : p.reserved)
            if (r)
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        frame.completion_label = p.release_label;
        frame.sequence = a[2];
        if (auto error = ReadEyePair(frame, p.textures, p.sampler, &p.tan_to_uv[0][0]))
            return error;
        if (overlay) {
            OrbisHmdReprojectionOverlayParam o{};
            if (!Read(a[3], o))
                return ORBIS_HMD_ERROR_PARAMETER_NULL;
            for (auto r : o.reserved)
                if (r)
                    return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            return ReadEyePair(frame, o.textures, o.sampler, &o.tan_to_uv[0][0]);
        }
        return 0;
    }

public:
    GuestReprojection(GuestCpu::GuestAddressSpace& space, Backend backend)
        : space(space), backend(std::move(backend)) {}

    u32 Dispatch(std::string_view nid, const std::array<u64, 6>& a, bool enabled) {
        using namespace Libraries::Hmd;
        using namespace GuestCpu;
        if (!enabled)
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        auto& s = *state;
        std::unique_lock lock(s.mutex);
        // Firmware allows event registration before reprojection Initialize.
        const bool start = nid == "7as0CjXW1B8" || nid == "mdyFbaJj66M";
        if (start || nid == "knyIhlkpLgE" || nid == "94+Ggm38KCg") {
            auto& event = start ? s.start : s.end;
            if (nid == "mdyFbaJj66M" || nid == "94+Ggm38KCg") {
                if (!event.queue)
                    return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_NOT_SET;
                event = {};
                return 0;
            }
            if (event.queue)
                return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_ALREADY_SET;
            if (!backend.event_exists(a[0], u32(a[1])))
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            event = {s64(a[0]), u32(a[1])};
            return 0;
        }
        if (nid == "OuygGEWkins") {
            OrbisHmdReprojectionInitializeParam p{};
            if (!Read(a[0], p) || !p.opaque0 || !p.opaque8)
                return ORBIS_HMD_ERROR_PARAMETER_NULL;
            if (a[2] || a[1] > 2 || p.opaque0 == p.opaque8 || (p.opaque0 & 255) ||
                (p.opaque8 & 255) || p.selector20 > 6 || p.selector24 > 7 || p.reserved28 ||
                p.reserved2c || p.reserved30)
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            if (s.initialized)
                return ORBIS_HMD_ERROR_REPROJECTION_ALREADY_INITIALIZED;
            // Firmware +0x86d0 identifies +0 as the 0x810 onion work area and
            // +8 as the 1 MiB garlic area. The passthrough does not allocate a
            // second GPU job or keep these work areas pinned while idle.
            if (!ValidateWorkAreas(p.opaque0, p.opaque8))
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            s.initialized = true;
            s.stopped = false;
            ++s.epoch;
            return 0;
        }
        if (!s.initialized)
            return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
        if (nid == "E+dPfjeQLHI") {
            if (a[3] || s32(a[1]) == s32(a[2]))
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            if (s.display)
                return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_ALREADY_SET;
            if (auto error = backend.validate_display(s32(a[0]), s32(a[1]), s32(a[2])))
                return u32(error);
            s.video = s32(a[0]);
            s.indices[0] = s32(a[1]);
            s.indices[1] = s32(a[2]);
            s.next = 0;
            s.display = true;
            s.stopped = false;
            // Ordinary VideoOut flips use the same port as the PSVR display
            // buffers. While reprojection is active they must not enqueue a
            // competing presentation that can overwrite the direct SBS frame.
            backend.set_active(true);
            const auto weak = std::weak_ptr<State>(state);
            const auto trigger = backend.trigger;
            backend.set_cadence([weak, trigger] {
                if (auto state = weak.lock()) {
                    std::lock_guard guard(state->mutex);
                    if (state->initialized && state->display && !state->stopped &&
                        state->start.queue)
                        trigger(state->start.queue, state->start.id);
                }
            });
            return 0;
        }
        if (nid == "vzMEkwBQciM" || nid == "ZrV5YIqD09I" || nid == "iGNNpDDjcwo") {
            s.stopped = true;
            backend.set_active(false);
            // Only our in-flight eye reads must retire. No queue/device-idle.
            if (!s.done.wait_for(lock, std::chrono::seconds(2), [&] { return s.pending == 0; }))
                return ORBIS_HMD_ERROR_REPROJECTION_THREAD_NOT_WORKING;
            if (nid != "vzMEkwBQciM")
                s.display = false;
            if (nid == "ZrV5YIqD09I") {
                s.initialized = false;
                s.start = {};
                s.end = {};
                ++s.epoch;
            }
            return 0;
        }
        const bool legacy = nid == "dntZTJ7meIU" || nid == "q3e8+nEguyE" || nid == "kcldQ7zLYQQ";
        if (!legacy && nid != "8gH1aLgty5I")
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        if (!s.display)
            return ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER;
        if (s.pending >= 2)
            return ORBIS_HMD_ERROR_REPROJECTION_DISPLAY_BUFFER_BUSY;
        ReprojectionFrame frame{};
        const u32 slot = s.busy[s.next] ? (s.next ^ 1) : s.next;
        frame.video_handle = s.video;
        frame.display_index = s.indices[slot];
        if (legacy) {
            if (auto error = ReadLegacy(nid, a, frame))
                return error;
        } else {
            if (a[5] || !a[1] || a[1] > 3)
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            OrbisHmdReprojectionSubmission submit{};
            std::array<u8, 0x38> shared{};
            if (!Read(a[2], submit) || !Read(a[3], shared) || !submit.root00)
                return ORBIS_HMD_ERROR_PARAMETER_NULL;
            if ((submit.root00 & 7) || submit.selector08 < 2000 || submit.selector08 > 7000 ||
                submit.selector18 > 1 || (submit.flags20 & 0xffffffff0ffffff0ULL) ||
                (submit.selector08 < 3000 && (submit.flags20 & 1)))
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            for (auto reserved : submit.reserved28)
                if (reserved)
                    return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            // Supported first-stage compositor: one colour layer and optional colour
            // overlay. Depth reprojection/extra layers stay explicit unsupported.
            if (a[1] > 2)
                return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
            frame.completion_label = submit.root00;
            frame.sequence = a[4];
            for (u32 i = 0; i < a[1]; ++i) {
                OrbisHmdReprojectionLayer layer{};
                if (a[0] > UINT64_MAX - (i + 1) * sizeof(layer) ||
                    !Read(a[0] + i * sizeof(layer), layer))
                    return ORBIS_HMD_ERROR_PARAMETER_NULL;
                if (layer.kind78 != (i ? 2u : 0u))
                    return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
                if (layer.roots00[2] || layer.roots00[3] || layer.reserved_a0)
                    return ORBIS_HMD_ERROR_PARAMETER_INVALID;
                for (auto reserved : layer.reserved7c)
                    if (reserved)
                        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
                // This optional 0x88-byte record configures PSVR warp. The SBS
                // transport validates its wire shape but samples the original eyes.
                if (layer.optional70) {
                    std::array<u64, 17> warp{};
                    if (!Read(layer.optional70, warp))
                        return ORBIS_HMD_ERROR_PARAMETER_NULL;
                    if (i || u32(warp[5]) > 1 || (warp[5] >> 32) || (u32(warp[5]) == 0 && !warp[4]))
                        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
                    for (u32 w = 6; w < warp.size(); ++w)
                        if (warp[w])
                            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
                }
                if (auto error = ReadEyePair(frame, layer.roots00, layer.root20,
                                            &layer.color_tan_to_uv[0][0]))
                    return error;
            }
        }
        const auto epoch = s.epoch;
        const auto weak = std::weak_ptr<State>(state);
        const auto trigger = backend.trigger;
        auto once = std::make_shared<std::atomic_bool>(false);
        auto done = [weak, epoch, trigger, slot, once](bool retired) {
            if (once->exchange(true))
                return;
            if (auto state = weak.lock()) {
                std::lock_guard guard(state->mutex);
                if (state->epoch != epoch)
                    return;
                --state->pending;
                state->busy[slot] = false;
                if (retired) {
                    ++state->completed;
                    if (state->end.queue)
                        trigger(state->end.queue, state->end.id);
                }
                state->done.notify_all();
            }
        };
        // Construct potentially allocating callback state before claiming a slot.
        std::function<void(bool)> completion = done;
        backend.set_active(true);
        ++s.pending;
        s.busy[slot] = true;
        s.next = slot ^ 1;
        s.stopped = false;
        lock.unlock();
        s32 result{};
        try {
            result = backend.submit(frame, std::move(completion));
        } catch (...) {
            done(false);
            throw;
        }
        if (result)
            done(false);
        else {
            lock.lock();
            ++s.submitted;
        }
        return u32(result);
    }
};
} // namespace Core::HostRuntime
