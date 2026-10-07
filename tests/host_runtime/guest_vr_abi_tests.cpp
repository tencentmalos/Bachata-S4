// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cstdio>
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/vr_tracker/vr_tracker_play_area.h"
#include "core/libraries/vr_tracker/vr_tracker_devices.h"
#include "core/host_runtime/vr_geometry.h"
#include "core/host_runtime/vr_time.h"
#include <algorithm>
#include <cmath>

using namespace Libraries::VrTracker;

int main() {
    unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
    using Bytes = std::array<u8, 0x40>;
    OrbisVrTrackerPlayAreaWarningInfo info{};
    CHECK(PlayAreaWarningInfoNoProvider(false, nullptr) == ORBIS_VR_TRACKER_ERROR_NOT_INIT);
    CHECK(PlayAreaWarningInfoNoProvider(false, &info) == ORBIS_VR_TRACKER_ERROR_NOT_INIT);
    CHECK(PlayAreaWarningInfoNoProvider(true, nullptr) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
    for (u32 size : {0u, 0x3fu, 0x41u, 0xffffffffu}) {
        info.size = size;
        const auto before = std::bit_cast<Bytes>(info);
        CHECK(PlayAreaWarningInfoNoProvider(true, &info) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        CHECK(std::bit_cast<Bytes>(info) == before);
    }
    info.size = sizeof(info);
    const auto baseline = std::bit_cast<Bytes>(info);
    // Firmware validates every reserved byte. Check each independently,
    // including padding adjacent to the two byte-sized output flags.
    for (size_t offset = 4; offset < baseline.size(); ++offset) {
        const bool reserved = offset != 0x10 && offset != 0x20 &&
                              !(offset >= 0x24 && offset < 0x2c);
        auto bytes = baseline;
        bytes[offset] = 0xff;
        auto candidate = std::bit_cast<OrbisVrTrackerPlayAreaWarningInfo>(bytes);
        CHECK(PlayAreaWarningInfoNoProvider(true, &candidate) ==
              (reserved ? ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID :
                          ORBIS_VR_TRACKER_ERROR_NOT_SUPPORTED));
        CHECK(std::bit_cast<Bytes>(candidate) == bytes);
    }
    CHECK(PlayAreaWarningInfoNoProvider(true, &info) == ORBIS_VR_TRACKER_ERROR_NOT_SUPPORTED);
    CHECK(std::bit_cast<Bytes>(info) == baseline);

    // DualShock 4 registration: up to four, colours handed out in order, the first is the
    // player's, order kept on unregistration.
    {
        PadRegistry pads;
        OrbisVrTrackerLedColor color{};
        CHECK(pads.Empty());
        CHECK(pads.Register(10, -1, &color) == ORBIS_OK && color == ORBIS_VR_TRACKER_LED_COLOR_BLUE);
        CHECK(pads.Register(10, -1) == ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED);
        CHECK(pads.Register(11, 3, &color) == ORBIS_OK && color == ORBIS_VR_TRACKER_LED_COLOR_MAGENTA);
        CHECK(pads.Register(12, -1, &color) == ORBIS_OK && color == ORBIS_VR_TRACKER_LED_COLOR_RED);
        CHECK(pads.Register(13, -1, &color) == ORBIS_OK && static_cast<s32>(color) == 2);
        CHECK(pads.Register(14, -1) == ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED);
        CHECK(pads.IsPlayers(10) && !pads.IsPlayers(11));
        CHECK(pads.Find(12) && pads.Find(12)->color == ORBIS_VR_TRACKER_LED_COLOR_RED);
        CHECK(!pads.Find(99));
        CHECK(pads.Unregister(10) && !pads.Unregister(10));
        CHECK(pads.IsPlayers(11));
        CHECK(pads.Register(15, -1, &color) == ORBIS_OK && color == ORBIS_VR_TRACKER_LED_COLOR_BLUE);
        pads.Clear();
        CHECK(pads.Empty() && !pads.IsPlayers(11));
    }

    // Recalibration: CALIBRATING for 200 ms after Recalibrate, and at least once however late
    // the title asks; then the normal status again. Kinds are independent.
    {
        Recalibrations recal;
        const auto dual = ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4;
        const auto hmd = ORBIS_VR_TRACKER_DEVICE_HMD;
        CHECK(!recal.Report(dual, 0));
        CHECK(!recal.Report(hmd, 5'000'000));
        recal.Begin(dual, 1'000'000);
        CHECK(!recal.Report(hmd, 1'000'001));
        CHECK(recal.Report(dual, 1'000'001));
        CHECK(recal.Report(dual, 1'000'000 + Recalibrations::Duration - 1));
        CHECK(!recal.Report(dual, 1'000'000 + Recalibrations::Duration));
        CHECK(!recal.Report(dual, 9'000'000));
        // Asked only after the window: one CALIBRATING result, then the normal status.
        recal.Begin(dual, 2'000'000);
        CHECK(recal.Report(dual, 3'000'000));
        CHECK(!recal.Report(dual, 3'000'001));
        // A new request restarts the window.
        recal.Begin(hmd, 4'000'000);
        recal.Begin(hmd, 4'100'000);
        CHECK(recal.Report(hmd, 4'250'000));
        CHECK(!recal.Report(hmd, 4'300'000));
        recal.Begin(dual, 5'000'000);
        recal.Clear();
        CHECK(!recal.Report(dual, 5'000'001));
    }

    // Host sample times (steady ns) become process microseconds, keeping their age or lead.
    {
        using Core::HostRuntime::VrTime::SteadyNsToProcessUs;
        CHECK(SteadyNsToProcessUs(0, 10'000'000'000, 777) == 777);
        CHECK(SteadyNsToProcessUs(9'990'000'000, 10'000'000'000, 1'000'000) == 990'000);
        CHECK(SteadyNsToProcessUs(10'011'000'000, 10'000'000'000, 1'000'000) == 1'011'000);
        CHECK(SteadyNsToProcessUs(1, 10'000'000'000, 1'000) == 0);
    }

    // A Move held level reads gravity on +Y; turned on its side, on X.
    {
        Core::HostRuntime::GuestVrSensor::Pose pose{};
        pose.orientation_valid = true;
        auto a = Core::HostRuntime::VrGeometry::RestingAccelerometer(pose, 9.81f);
        CHECK(std::abs(a[0]) < 1e-5f && std::abs(a[1] - 9.81f) < 1e-5f && std::abs(a[2]) < 1e-5f);
        const float h = std::sqrt(0.5f);
        pose.orientation = {0, 0, h, h}; // 90 degrees about Z: body +X now points up
        a = Core::HostRuntime::VrGeometry::RestingAccelerometer(pose, 9.81f);
        CHECK(std::abs(a[0] - 9.81f) < 1e-4f && std::abs(a[1]) < 1e-4f && std::abs(a[2]) < 1e-4f);
    }

    // Canted eyes. The title renders a parallel view (head orientation). Presenting it as the
    // canted eye's view shows every direction theta off; the parallel envelope view (head
    // orientation, enclosing field of view) shows each where it is.
    {
        namespace G = Core::HostRuntime::VrGeometry;
        const float deg = 3.14159265f / 180.f;
        const std::array<float, 4> fov{-50 * deg, 45 * deg, -50 * deg, 48 * deg};
        const auto angle = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            // atan2 of |a x b| and a . b: exact near 0, where acos of a cosine is not.
            const float cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2],
                        cz = a[0] * b[1] - a[1] * b[0];
            return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz),
                              a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        };
        for (const float cant_deg : {0.f, 5.f, -5.f, 10.f, -10.f}) {
            const float theta = cant_deg * deg;
            const std::array<float, 4> cant{0, std::sin(theta / 2), 0, std::cos(theta / 2)};
            CHECK(std::abs(G::CantAngle(cant) - std::abs(theta)) < 1e-4f);
            const auto envelope = G::ParallelEnvelopeFov(cant, fov);
            CHECK(envelope.has_value() && G::ValidFov(*envelope));
            if (!envelope)
                continue;
            if (cant_deg == 0)
                for (unsigned i = 0; i < 4; ++i)
                    CHECK(std::abs((*envelope)[i] - fov[i]) < 1e-5f);
            float worst_eye = 0, worst_parallel = 0;
            for (float tx : {std::tan(fov[0]) * .99f, 0.f, std::tan(fov[1]) * .99f}) {
                for (float ty : {std::tan(fov[2]) * .99f, 0.f, std::tan(fov[3]) * .99f}) {
                    // A pixel of the canted eye, as a direction in the head's frame.
                    const auto shown = G::Rotate(cant, {tx, ty, -1});
                    // Eye view: the title's image is sampled at the eye-frame tangents,
                    // i.e. the content of the head-frame direction (tx, ty, -1).
                    worst_eye = std::max(worst_eye, angle(shown, {tx, ty, -1}));
                    // Parallel view: the compositor samples at the head-frame tangents of
                    // `shown`, which the envelope has to contain; the content is `shown`.
                    const float hx = -shown[0] / shown[2], hy = -shown[1] / shown[2];
                    CHECK(hx >= std::tan((*envelope)[0]) - 1e-5f &&
                          hx <= std::tan((*envelope)[1]) + 1e-5f &&
                          hy >= std::tan((*envelope)[2]) - 1e-5f &&
                          hy <= std::tan((*envelope)[3]) + 1e-5f);
                    worst_parallel = std::max(worst_parallel, angle(shown, {hx, hy, -1}));
                }
            }
            // The eye view is off by the whole cant; the parallel one not at all.
            CHECK(std::abs(worst_eye - std::abs(theta)) < 0.01f * deg + std::abs(theta) * 0.02f);
            CHECK(worst_parallel < 1e-4f);
            // Only canted eyes get the parallel view.
            CHECK((std::abs(theta) > G::ParallelEnvelopeCant) == (cant_deg != 0));
        }
        // Pitched optics are enclosed too; an eye turned past the frustum is refused.
        const std::array<float, 4> pitch{std::sin(4 * deg), 0, 0, std::cos(4 * deg)};
        const auto pitched = G::ParallelEnvelopeFov(pitch, fov);
        CHECK(pitched && (*pitched)[3] > fov[3] && (*pitched)[2] > fov[2]);
        const std::array<float, 4> sideways{0, std::sin(45 * deg), 0, std::cos(45 * deg)};
        CHECK(!G::ParallelEnvelopeFov(sideways, fov));
    }

    std::printf("GUEST_VR_ABI checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
