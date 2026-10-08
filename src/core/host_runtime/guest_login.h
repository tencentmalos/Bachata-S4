// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <mutex>
#include <string_view>
#include "common/types.h"

namespace Core::HostRuntime {
// LoginService and LoginDialog are local console services, distinct from PSN
// SigninDialog. Firmware 11.00 initializes their client state without IPC:
// LoginService +0x1150/+0x1190, LoginDialog +0x11b0/+0x11f0. Only the verified
// lifecycle/status surface is admitted here; device requests and interactive
// login remain unsupported until they have a checked guest-data bridge.
class GuestLogin final {
public:
    static constexpr std::array<std::string_view, 2> ServiceNids{
        "T9-NuSHAvcQ", "aR8+Hvghm0E"};
    static constexpr std::array<std::string_view, 4> DialogNids{
        "qP-EvQRl2Hc", "vMQJRUKsf3U", "2rc+egSfb5A", "HAiWUEwEfGo"};
    static bool IsService(std::string_view nid) {
        return std::ranges::find(ServiceNids, nid) != ServiceNids.end();
    }
    static bool IsDialog(std::string_view nid) {
        return std::ranges::find(DialogNids, nid) != DialogNids.end();
    }
    static bool IsNid(std::string_view nid) {
        return IsService(nid) || IsDialog(nid);
    }
    static bool Admits(std::string_view nid, std::string_view suffix) {
        return (IsService(nid) && suffix == "#libSceLoginService#1#libSceLoginService#Function") ||
               (IsDialog(nid) && suffix == "#libSceLoginDialog#1#libSceLoginDialog#Function");
    }
    u32 Invoke(std::string_view nid) {
        std::lock_guard lock(mutex);
        if (nid == "T9-NuSHAvcQ")
            return Initialize(service, 0x813a0002);
        if (nid == "aR8+Hvghm0E")
            return Terminate(service, 0x813a0001);
        if (nid == "qP-EvQRl2Hc")
            return Initialize(dialog, 0x81340002);
        if (nid == "vMQJRUKsf3U")
            return Terminate(dialog, 0x81340001);
        if (nid == "2rc+egSfb5A" || nid == "HAiWUEwEfGo")
            return dialog ? 1 : 0;
        return 0x81340003;
    }

private:
    static u32 Initialize(bool& state, u32 already) {
        if (state)
            return already;
        state = true;
        return 0;
    }
    static u32 Terminate(bool& state, u32 uninitialized) {
        if (!state)
            return uninitialized;
        state = false;
        return 0;
    }
    std::mutex mutex;
    bool service{};
    bool dialog{};
};
} // namespace Core::HostRuntime
