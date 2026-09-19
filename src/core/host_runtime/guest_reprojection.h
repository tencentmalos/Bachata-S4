// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "video_core/amdgpu/resource.h"

namespace Core::HostRuntime {
// SBS transport only: no optical distortion or pose/time warp. Guest records
// are copied at admission; the renderer owns the GPU read leases until retirement.
struct ReprojectionFrame {
    std::array<AmdGpu::Image, 4> eyes{}; // base L/R, optional overlay L/R
    u32 image_count{};
    u64 completion_label{}, sequence{};
    s32 video_handle{}, display_index{};
};
inline constexpr std::string_view ReprojectionNids[]{
    "OuygGEWkins", "ZrV5YIqD09I", "vzMEkwBQciM", "E+dPfjeQLHI", "iGNNpDDjcwo",
    "7as0CjXW1B8", "knyIhlkpLgE", "mdyFbaJj66M", "94+Ggm38KCg", "8gH1aLgty5I"};

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
    struct Event { s64 queue{}; u32 id{}; };
    struct State {
        std::mutex mutex;
        std::condition_variable done;
        bool initialized{}, display{}, stopped{};
        s32 video{}, indices[2]{};
        u32 next{}, pending{};
        u64 epoch{}, submitted{}, completed{};
        Event start{}, end{};
    };
    GuestCpu::GuestAddressSpace& space;
    Backend backend;
    std::shared_ptr<State> state = std::make_shared<State>();
    template <typename T> bool Read(u64 address, T& value) {
        return address && bool(space.ReadData(GuestCpu::GuestAddress{address},
                                              std::as_writable_bytes(std::span{&value, 1})));
    }
public:
    GuestReprojection(GuestCpu::GuestAddressSpace& space, Backend backend)
        : space(space), backend(std::move(backend)) {}

    u32 Dispatch(std::string_view nid, const std::array<u64, 6>& a, bool enabled) {
        using namespace Libraries::Hmd;
        using namespace GuestCpu;
        if (!enabled) return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        auto& s = *state;
        std::unique_lock lock(s.mutex);
        // Firmware allows event registration before reprojection Initialize.
        const bool start = nid == "7as0CjXW1B8" || nid == "mdyFbaJj66M";
        if (start || nid == "knyIhlkpLgE" || nid == "94+Ggm38KCg") {
            auto& event = start ? s.start : s.end;
            if (nid == "mdyFbaJj66M" || nid == "94+Ggm38KCg") {
                if (!event.queue) return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_NOT_SET;
                event = {}; return 0;
            }
            if (event.queue) return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_ALREADY_SET;
            if (!backend.event_exists(a[0], u32(a[1]))) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            event = {s64(a[0]), u32(a[1])}; return 0;
        }
        if (nid == "OuygGEWkins") {
            OrbisHmdReprojectionInitializeParam p{};
            if (!Read(a[0], p) || !p.opaque0 || !p.opaque8) return ORBIS_HMD_ERROR_PARAMETER_NULL;
            if (a[2] || a[1] > 2 || p.opaque0 == p.opaque8 || (p.opaque0 & 255) ||
                (p.opaque8 & 255) || p.selector20 > 6 || p.selector24 > 7 ||
                p.reserved28 || p.reserved2c || p.reserved30) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            if (s.initialized) return ORBIS_HMD_ERROR_REPROJECTION_ALREADY_INITIALIZED;
            // Firmware +0x86d0 identifies +0 as the 0x810 onion work area and
            // +8 as the 1 MiB garlic area. The passthrough does not allocate a
            // second GPU job or keep these work areas pinned while idle.
            std::array requests{
                GuestAddressSpace::DataRequest{{GuestAddress{p.opaque0}, 0x810}, GuestPermission::Write},
                GuestAddressSpace::DataRequest{{GuestAddress{p.opaque8}, 0x100000}, GuestPermission::Write}};
            if (!space.AcquireDataBatch(requests)) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            s.initialized = true; s.stopped = false; ++s.epoch;
            return 0;
        }
        if (!s.initialized) return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
        if (nid == "E+dPfjeQLHI") {
            if (a[3] || s32(a[1]) == s32(a[2])) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            if (s.display) return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_ALREADY_SET;
            if (auto error = backend.validate_display(s32(a[0]), s32(a[1]), s32(a[2]))) return u32(error);
            s.video = s32(a[0]); s.indices[0] = s32(a[1]); s.indices[1] = s32(a[2]);
            s.next = 0; s.display = true; s.stopped = false;
            // Ordinary VideoOut flips use the same port as the PSVR display
            // buffers. While reprojection is active they must not enqueue a
            // competing presentation that can overwrite the direct SBS frame.
            backend.set_active(true);
            const auto weak = std::weak_ptr<State>(state);
            const auto trigger = backend.trigger;
            backend.set_cadence([weak, trigger] {
                if (auto state = weak.lock()) {
                    std::lock_guard guard(state->mutex);
                    if (state->initialized && state->display && !state->stopped && state->start.queue)
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
            if (nid != "vzMEkwBQciM") s.display = false;
            if (nid == "ZrV5YIqD09I") {
                s.initialized = false; s.start = {}; s.end = {}; ++s.epoch;
            }
            return 0;
        }
        if (nid != "8gH1aLgty5I") return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        if (!s.display) return ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER;
        if (s.pending >= 2) return ORBIS_HMD_ERROR_REPROJECTION_DISPLAY_BUFFER_BUSY;
        if (a[5] || !a[1] || a[1] > 3) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        OrbisHmdReprojectionSubmission submit{};
        std::array<u8, 0x38> shared{};
        if (!Read(a[2], submit) || !Read(a[3], shared) || !submit.root00)
            return ORBIS_HMD_ERROR_PARAMETER_NULL;
        if ((submit.root00 & 7) || submit.selector08 < 2000 || submit.selector08 > 7000 ||
            submit.selector18 > 1 || (submit.flags20 & 0xffffffff0ffffff0ULL) ||
            (submit.selector08 < 3000 && (submit.flags20 & 1))) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        for (auto reserved : submit.reserved28) if (reserved) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        // Supported first-stage compositor: one colour layer and optional colour
        // overlay. Depth reprojection/extra layers stay explicit unsupported.
        if (a[1] > 2) return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        ReprojectionFrame frame{};
        frame.completion_label = submit.root00; frame.sequence = a[4];
        frame.video_handle = s.video; frame.display_index = s.indices[s.next];
        for (u32 i = 0; i < a[1]; ++i) {
            OrbisHmdReprojectionLayer layer{};
            std::array<u8, 16> scale{};
            if (!Read(a[0] + i * sizeof(layer), layer) || !Read(layer.root20, scale))
                return ORBIS_HMD_ERROR_PARAMETER_NULL;
            if (layer.kind78 != (i ? 2u : 0u)) return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
            if (layer.roots00[2] || layer.roots00[3] || layer.reserved_a0)
                return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            for (auto reserved : layer.reserved7c) if (reserved) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            // This optional 0x88-byte record configures PSVR warp. The SBS
            // transport validates its wire shape but samples the original eyes.
            if (layer.optional70) {
                std::array<u64, 17> warp{};
                if (!Read(layer.optional70, warp)) return ORBIS_HMD_ERROR_PARAMETER_NULL;
                if (i || u32(warp[5]) > 1 || (warp[5] >> 32) ||
                    (u32(warp[5]) == 0 && !warp[4])) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
                for (u32 w = 6; w < warp.size(); ++w)
                    if (warp[w]) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
            }
            for (u32 eye = 0; eye < 2; ++eye)
                if (!Read(layer.roots00[eye], frame.eyes[frame.image_count++]))
                    return ORBIS_HMD_ERROR_PARAMETER_NULL;
        }
        ++s.pending; s.next ^= 1; s.stopped = false;
        const auto epoch = s.epoch;
        const auto weak = std::weak_ptr<State>(state);
        const auto trigger = backend.trigger;
        auto done = [weak, epoch, trigger](bool retired) {
            if (auto state = weak.lock()) {
                std::lock_guard guard(state->mutex);
                if (state->epoch != epoch) return;
                --state->pending;
                if (retired) {
                    ++state->completed;
                    if (state->end.queue) trigger(state->end.queue, state->end.id);
                }
                state->done.notify_all();
            }
        };
        lock.unlock();
        s32 result{};
        try { result = backend.submit(frame, done); }
        catch (...) { done(false); throw; }
        if (result) done(false);
        else { lock.lock(); ++s.submitted; }
        return u32(result);
    }
};
} // namespace Core::HostRuntime
