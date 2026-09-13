// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/sysmodule/sysmodule_error.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/systemservice_error.h"
#include "core/libraries/system/userservice_error.h"

namespace Core::HostRuntime {
// Providers are published by successful initialization, never by DT_NEEDED.
// Statically loaded guest dependencies remain pinned until generation teardown;
// these references govern API visibility, not dynamic ELF/TLS unload.
class GuestSysmodules {
public:
    using Lookup = std::function<std::optional<std::string>(u32)>;
    explicit GuestSysmodules(Lookup lookup) : lookup(std::move(lookup)) {}
    void Publish(std::string name, s32 handle) {
        std::lock_guard lock(mutex);
        if (!providers.emplace(std::move(name), handle).second)
            throw std::logic_error("duplicate sysmodule provider");
    }
    s32 Load(u32 id) {
        const auto name = lookup(id);
        if (!name)
            return ORBIS_SYSMODULE_INVALID_ID;
        std::lock_guard lock(mutex);
        const auto provider = providers.find(*name);
        if (provider == providers.end())
            return ORBIS_SYSMODULE_LOCK_FAILED;
        auto& count = references[id];
        if (count == std::numeric_limits<u32>::max())
            return ORBIS_SYSMODULE_LOCK_FAILED;
        ++count;
        return 0;
    }
    s32 Unload(u32 id) {
        if (!lookup(id))
            return ORBIS_SYSMODULE_INVALID_ID;
        std::lock_guard lock(mutex);
        auto it = references.find(id);
        if (it == references.end())
            return ORBIS_SYSMODULE_NOT_LOADED;
        if (--it->second == 0)
            references.erase(it);
        return 0;
    }
    s32 Handle(u32 id, s32* out = nullptr) const {
        const auto name = lookup(id);
        if (!name)
            return ORBIS_SYSMODULE_INVALID_ID;
        std::lock_guard lock(mutex);
        if (!references.contains(id))
            return ORBIS_SYSMODULE_NOT_LOADED;
        if (out)
            *out = providers.at(*name);
        return 0;
    }

private:
    Lookup lookup;
    mutable std::mutex mutex;
    std::map<std::string, s32> providers;
    std::map<u32, u32> references;
};

struct GuestUser {
    s32 id{-1};
    std::string name;
};

// A copied local-user profile and event state per runtime, independent of desktop
// queues, NP callbacks and mutable UserManager storage. No credentials are copied.
class GuestPlatform {
public:
    GuestPlatform(std::array<GuestUser, 4> users, u32 sdk, s32 language, bool circle)
        : users(std::move(users)), sdk(sdk), language(language), circle(circle) {}
    s32 Initialize() {
        std::lock_guard lock(mutex);
        if (initialized)
            return ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED;
        if (users[0].id < 0)
            return ORBIS_USER_SERVICE_ERROR_NOT_LOGGED_IN;
        initialized = true;
        user_events.clear();
        for (const auto& user : users)
            if (user.id >= 0)
                user_events.push_back(
                    {Libraries::UserService::OrbisUserServiceEventType::Login, user.id});
        return 0;
    }
    s32 Terminate() {
        std::lock_guard lock(mutex);
        if (!initialized)
            return ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED;
        initialized = false;
        user_events.clear();
        return 0;
    }
    s32 InitialUser(s32& id) const {
        std::lock_guard lock(mutex);
        if (!initialized)
            return ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED;
        id = users[0].id;
        return 0;
    }
    s32 LoginUsers(Libraries::UserService::OrbisUserServiceLoginUserIdList& out) const {
        std::lock_guard lock(mutex);
        if (!initialized)
            return ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED;
        for (size_t i = 0; i < users.size(); ++i)
            out.user_id[i] = users[i].id;
        return 0;
    }
    s32 UserName(s32 id, std::string& out) const {
        std::lock_guard lock(mutex);
        if (!initialized)
            return ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED;
        for (const auto& user : users)
            if (id >= 0 && user.id == id) {
                out = user.name;
                return 0;
            }
        return ORBIS_USER_SERVICE_ERROR_NOT_LOGGED_IN;
    }
    s32 UserEvent(Libraries::UserService::OrbisUserServiceEvent& out) {
        std::lock_guard lock(mutex);
        if (!initialized)
            return ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED;
        if (user_events.empty())
            return ORBIS_USER_SERVICE_ERROR_NO_EVENT;
        out = user_events.front();
        user_events.pop_front();
        return 0;
    }
    s32 Param(Libraries::SystemService::OrbisSystemServiceParamId id, s32& out) const;
    Libraries::SystemService::OrbisSystemServiceStatus Status() const {
        std::lock_guard lock(mutex);
        return {.event_num = static_cast<s32>(system_events.size()),
                .is_in_background_execution = background,
                .is_cpu_mode7_cpu_normal = true};
    }
    s32 SystemEvent(Libraries::SystemService::OrbisSystemServiceEvent& out) {
        std::lock_guard lock(mutex);
        if (system_events.empty())
            return ORBIS_SYSTEM_SERVICE_ERROR_NO_EVENT;
        out = system_events.front();
        system_events.pop_front();
        return 0;
    }
    void SetBackground(bool value) {
        std::lock_guard lock(mutex);
        if (background && !value && system_events.size() < 64)
            system_events.push_back(
                {Libraries::SystemService::OrbisSystemServiceEventType::OnResume, {}});
        background = value;
    }
    void HideSplash() {
        std::lock_guard lock(mutex);
        splash = false;
    }
    bool SplashVisible() const {
        std::lock_guard lock(mutex);
        return splash;
    }

private:
    const std::array<GuestUser, 4> users;
    const u32 sdk;
    const s32 language;
    const bool circle;
    mutable std::mutex mutex;
    bool initialized{}, background{}, splash{true};
    std::deque<Libraries::UserService::OrbisUserServiceEvent> user_events;
    std::deque<Libraries::SystemService::OrbisSystemServiceEvent> system_events;
};

inline s32 GuestPlatform::Param(Libraries::SystemService::OrbisSystemServiceParamId id,
                                s32& out) const {
    using namespace Libraries::SystemService;
    switch (id) {
    case OrbisSystemServiceParamId::Lang:
        out = language;
        if (out == 0x15 && sdk < 0x02000000)
            out = 0x12;
        if (out == 0x16 && sdk < 0x02500000)
            out = 2;
        if (out >= 0x17 && out <= 0x1a && sdk < 0x05000000)
            out = 0x12;
        if (out >= 0x1b && out <= 0x1d && sdk < 0x05000000)
            out = 1;
        if (out == 0x1e && sdk < 0x10000000)
            out = 0x12;
        break;
    case OrbisSystemServiceParamId::DateFormat:
        out = 1;
        break;
    case OrbisSystemServiceParamId::TimeFormat:
        out = 1;
        break;
    // Same UTC profile as the production gettimeofday adapter.
    case OrbisSystemServiceParamId::TimeZone:
    case OrbisSystemServiceParamId::Summertime:
    case OrbisSystemServiceParamId::GameParentalLevel:
        out = 0;
        break;
    case OrbisSystemServiceParamId::EnterButtonAssign:
        out = circle ? 0 : 1;
        break;
    default:
        return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;
    }
    return 0;
}
} // namespace Core::HostRuntime
