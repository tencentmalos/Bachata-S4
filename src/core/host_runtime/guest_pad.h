// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_platform.h"
#include "core/host_runtime/orbis_pad_adapter.h"
#include "core/libraries/pad/pad_errors.h"

namespace Core::HostRuntime {
static_assert(sizeof(Libraries::Pad::OrbisPadData) == 120);
static_assert(offsetof(Libraries::Pad::OrbisPadData, timestamp) == 80);
inline constexpr std::string_view PadNids[]{
    "hv1luiJrqQM", "xk0AcarP3V4", "WFIiSfXGUq8", "u1GRHp+oWoY", "6ncge5+l5Qs",
    "AcslpN1jHR8", "IHPqcbc0zCA",
    "q1cHNfGycLI", "YndgXqQVV7c", "gjP9-KQzoUk", "hGbf2QTBmqc", "yFVnOdGxvZY",
    "RR4novUEENY", "DscD1i9HX1w", "rIZnR6eSpvk", "clVvL4ZDntw", "r44mAxdSG+U",
    "vDLMoJLde8I"};
inline bool IsPadNid(std::string_view nid) {
    return std::ranges::find(PadNids, nid) != std::end(PadNids);
}
// Checked guest ABI only. Device state/normalized input/haptics remain in the
// same host-DSO OrbisPadAdapter/InputHub consumed by application JNI. The app
// owns Begin/EndSession; production Stop drains guest calls before input teardown.
class GuestPad {
public:
    GuestPad(OrbisPadAdapter& pad, GuestPlatform& platform) : pad(pad), platform(platform) {}
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a) {
        using namespace GuestCpu;
        using namespace Libraries::Pad;
        std::lock_guard lock(mutex);
        const auto current = pad.CurrentToken();
        if (!current || (token && token != current))
            return ORBIS_PAD_ERROR_NOT_INITIALIZED;
        if (nid == "hv1luiJrqQM") {
            const int result = pad.Initialize();
            if (!result) token = current;
            return result;
        }
        if (!token)
            return ORBIS_PAD_ERROR_NOT_INITIALIZED;
        auto read = [&](u64 address, auto& out) {
            return bool(
                space.ReadData(GuestAddress{address}, std::as_writable_bytes(std::span{&out, 1})));
        };
        if (nid == "AcslpN1jHR8") {
            if (!space.ValidateRange(
                    {GuestAddress{a[1]}, sizeof(OrbisPadDeviceClassExtendedInformation)},
                    GuestPermission::Write))
                return ORBIS_PAD_ERROR_INVALID_ARG;
            // Android exposes the session's virtual pad as a standard
            // controller. The desktop implementation reads global controller
            // state, so publish only the ABI-safe class record here.
            OrbisPadDeviceClassExtendedInformation info{};
            info.deviceClass = OrbisPadDeviceClass::Standard;
            if (!space.WriteData(GuestAddress{a[1]},
                                 std::as_bytes(std::span{&info, size_t(1)})))
                return ORBIS_PAD_ERROR_INVALID_ARG;
            return 0;
        }
        if (nid == "IHPqcbc0zCA") {
            // Beat Saber probes this parser while bringing up the virtual
            // controller.  Android only exposes the standard pad class; keep
            // the desktop ABI contract (validated input/output records) and
            // report that no special-class payload is available.
            OrbisPadControllerInformation info{};
            const int info_result = pad.Information(s32(a[0]), &info);
            if (info_result)
                return info_result;
            if (a[1] && !space.ValidateRange({GuestAddress{a[1]}, sizeof(OrbisPadData)},
                                              GuestPermission::Read))
                return ORBIS_PAD_ERROR_INVALID_ARG;
            if (!a[2] || !space.ValidateRange({GuestAddress{a[2]}, sizeof(OrbisPadDeviceClassData)},
                                               GuestPermission::Write))
                return ORBIS_PAD_ERROR_INVALID_ARG;
            OrbisPadDeviceClassData value{};
            value.deviceClass = OrbisPadDeviceClass::Standard;
            value.bDataValid = false;
            if (!space.WriteData(GuestAddress{a[2]},
                                 std::as_bytes(std::span{&value, size_t(1)})))
                return ORBIS_PAD_ERROR_INVALID_ARG;
            return 0;
        }
        if (nid == "xk0AcarP3V4" || nid == "WFIiSfXGUq8") {
            const s32 user = a[0];
            if (user < 0 || user == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_SYSTEM)
                return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
            Libraries::UserService::OrbisUserServiceLoginUserIdList users{};
            if (platform.LoginUsers(users))
                return ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
            int port = -1;
            for (int i = 0; i < kMaxPadPorts; ++i)
                if (users.user_id[i] == user) port = i;
            if (port < 0)
                return ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
            const auto size = nid == "WFIiSfXGUq8" ? sizeof(OrbisPadOpenExtParam) : sizeof(OrbisPadOpenParam);
            // Desktop ignores these optional parameters; still reject an invalid guest span.
            if (a[3] && !space.ValidateRange({GuestAddress{a[3]}, size}, GuestPermission::Read))
                return ORBIS_PAD_ERROR_INVALID_ARG;
            return pad.Open(user, s32(a[1]), s32(a[2]), port);
        }
        if (nid == "u1GRHp+oWoY") return pad.GetHandle(s32(a[0]), s32(a[1]), s32(a[2]));
        if (nid == "6ncge5+l5Qs") return pad.Close(s32(a[0]));
        if (nid == "q1cHNfGycLI" || nid == "YndgXqQVV7c") {
            const bool latest = nid == "YndgXqQVV7c";
            const int count = latest ? 1 : s32(a[2]);
            if (count < 1 || count > ORBIS_PAD_MAX_DATA_NUM)
                return ORBIS_PAD_ERROR_INVALID_ARG;
            // Pin the entire advertised output before consuming history.
            auto pin = space.AcquireDataSpan(
                {GuestAddress{a[1]}, sizeof(OrbisPadData) * size_t(count)}, true);
            if (!pin) return ORBIS_PAD_ERROR_INVALID_ARG;
            std::array<OrbisPadData, ORBIS_PAD_MAX_DATA_NUM> data{};
            const int result = pad.Read(s32(a[0]), data.data(), count, latest, true);
            if (result < 0) return result;
            std::memcpy(pin.Value().WritableBytes().data(), data.data(), sizeof(OrbisPadData) * result);
            return latest ? 0 : result;
        }
        if (nid == "gjP9-KQzoUk" || nid == "hGbf2QTBmqc") {
            const bool extended = nid == "hGbf2QTBmqc";
            const size_t size = extended ? sizeof(OrbisPadExtendedControllerInformation) : sizeof(OrbisPadControllerInformation);
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, size}, true);
            if (!pin) return ORBIS_PAD_ERROR_INVALID_ARG;
            OrbisPadExtendedControllerInformation value{};
            const int result = pad.Information(s32(a[0]), &value.base);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, size);
            return result;
        }
        if (nid == "yFVnOdGxvZY") {
            OrbisPadVibrationParam value{};
            return read(a[1], value) ? pad.Vibrate(s32(a[0]), &value) : ORBIS_PAD_ERROR_INVALID_ARG;
        }
        OrbisPadControllerInformation info{};
        if (const int result = pad.Information(s32(a[0]), &info)) return result;
        if (nid == "RR4novUEENY") {
            OrbisPadLightBarParam value{};
            if (!read(a[1], value)) return ORBIS_PAD_ERROR_INVALID_ARG;
            if (value.r < 0xd && value.g < 0xd && value.b < 0xd)
                return ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING;
            return ORBIS_PAD_ERROR_NOT_PERMITTED; // Android backend has no lightbar actuator.
        }
        if (nid == "DscD1i9HX1w") return ORBIS_PAD_ERROR_NOT_PERMITTED;
        if (nid == "rIZnR6eSpvk") return 0; // Adapter currently publishes identity orientation only.
        // Same no-op compatibility controls as desktop. No sensor support is advertised.
        if (nid == "clVvL4ZDntw") return Libraries::Pad::scePadSetMotionSensorState(s32(a[0]), bool(a[1]));
        if (nid == "r44mAxdSG+U") return Libraries::Pad::scePadSetAngularVelocityDeadbandState(s32(a[0]), bool(a[1]));
        if (nid == "vDLMoJLde8I") return Libraries::Pad::scePadSetTiltCorrectionState(s32(a[0]), bool(a[1]));
        return ORBIS_PAD_ERROR_NOT_PERMITTED;
    }
private:
    OrbisPadAdapter& pad;
    GuestPlatform& platform;
    std::mutex mutex;
    u64 token{};
};
} // namespace Core::HostRuntime
