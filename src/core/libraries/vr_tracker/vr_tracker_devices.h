// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <optional>

#include "core/libraries/error_codes.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"

namespace Libraries::VrTracker {

// The DualShock 4s registered with the tracker, in registration order. A console's camera tells
// up to four apart by the colour of their light bars, and a title registers the controller of
// every logged-in player; the first one registered is the player's. Registration, lookups and
// unregistration come from different guest threads.
class PadRegistry {
public:
    struct Entry {
        s32 handle;
        OrbisVrTrackerLedColor color;
    };
    static constexpr std::size_t MaxPads = 4;
    // Colours a title may request, and the order free colours are handed out in.
    static constexpr s32 ColorCount = 5;

    // `requested_color` is the colour argument of sceVrTrackerRegisterDeviceInternal, negative
    // for "any free colour" (sceVrTrackerRegisterDevice/RegisterDevice2).
    s32 Register(s32 handle, s32 requested_color, OrbisVrTrackerLedColor* color_out = nullptr) {
        std::scoped_lock lock{mutex};
        for (std::size_t i = 0; i < count; ++i)
            if (entries[i].handle == handle)
                return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        if (count == MaxPads)
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        s32 color = requested_color;
        if (color < 0) {
            color = 0;
            while (color < ColorCount && UsedLocked(color))
                ++color;
        }
        entries[count++] = {handle, static_cast<OrbisVrTrackerLedColor>(color)};
        if (color_out)
            *color_out = static_cast<OrbisVrTrackerLedColor>(color);
        return ORBIS_OK;
    }

    bool Unregister(s32 handle) {
        std::scoped_lock lock{mutex};
        for (std::size_t i = 0; i < count; ++i) {
            if (entries[i].handle != handle)
                continue;
            // Keep the order: the first remaining controller becomes the player's.
            std::move(entries.begin() + i + 1, entries.begin() + count, entries.begin() + i);
            --count;
            return true;
        }
        return false;
    }

    std::optional<Entry> Find(s32 handle) const {
        std::scoped_lock lock{mutex};
        for (std::size_t i = 0; i < count; ++i)
            if (entries[i].handle == handle)
                return entries[i];
        return std::nullopt;
    }

    // Whether `handle` is the player's controller, the one a host pose source would track.
    bool IsPlayers(s32 handle) const {
        std::scoped_lock lock{mutex};
        return count != 0 && entries[0].handle == handle;
    }

    bool Empty() const {
        std::scoped_lock lock{mutex};
        return count == 0;
    }

    void Clear() {
        std::scoped_lock lock{mutex};
        count = 0;
    }

private:
    bool UsedLocked(s32 color) const {
        for (std::size_t i = 0; i < count; ++i)
            if (static_cast<s32>(entries[i].color) == color)
                return true;
        return false;
    }

    mutable std::mutex mutex;
    std::array<Entry, MaxPads> entries{};
    std::size_t count{};
};

// Asked to find a kind of device anew (sceVrTrackerRecalibrate), a console's tracker says for a
// moment that it is calibrating it, then reports it normally again. Titles wait for exactly that:
// ASTRO BOT Rescue Mission from 1.01 on asks for its controller to be found anew on the screen
// where the player sits inside a silhouette, and stays there until a result for the controller
// has gone from CALIBRATING to anything else (AstroQuest vr_tracker.cpp). The window lasts
// `Duration` and, however late the title asks, at least one result per device kind says so.
class Recalibrations {
public:
    static constexpr u64 Duration = 200'000; // process-time microseconds

    void Begin(OrbisVrTrackerDeviceType type, u64 now_us) {
        std::scoped_lock lock{mutex};
        windows[Index(type)] = {.until = now_us + Duration, .reported = false};
    }

    // Whether a result for this kind of device, produced at `now_us`, is to say CALIBRATING.
    bool Report(OrbisVrTrackerDeviceType type, u64 now_us) {
        std::scoped_lock lock{mutex};
        Window& window = windows[Index(type)];
        if (window.reported && now_us >= window.until)
            return false;
        window.reported = true;
        return true;
    }

    void Clear() {
        std::scoped_lock lock{mutex};
        windows = {};
    }

private:
    struct Window {
        u64 until{};
        bool reported{true};
    };
    static std::size_t Index(OrbisVrTrackerDeviceType type) {
        return std::min<std::size_t>(static_cast<std::size_t>(type), 3);
    }
    std::mutex mutex;
    std::array<Window, 4> windows{};
};

} // namespace Libraries::VrTracker
