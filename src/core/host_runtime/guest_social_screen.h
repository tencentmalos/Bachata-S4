// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libSceSocialScreen for the Android host: what the TV next to a PSVR headset shows.
//
// In mirror mode the TV shows the headset view; in separate mode the title renders the TV image
// itself and flips it to the social screen video output (SCE_VIDEO_OUT_BUS_TYPE_AUX_SOCIAL_SCREEN,
// handle 2 of the VideoOut driver, opened only while a headset is ready). That port accepts and
// completes flips but nothing is shown: there is no TV. This library keeps the mode and the
// separate-mode state a title sets; the sequence a title is expected to follow is checked.
//
// Source: AstroQuest v0.18 (9ff3e43) shadps4-arm64-main/src/core/libraries/social_screen, where
// every call succeeds. The firmware's error codes for this library are not known here, so a call
// out of sequence (uninitialized, opened twice, a null parameter) is refused by name and stops the
// session instead of returning an invented code. The parameter record (s32 mode, 28 reserved
// bytes) is AstroQuest's reading of ASTRO BOT's calls, not checked against the firmware.

#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"

namespace Core::HostRuntime {

class GuestSocialScreen {
public:
    static constexpr std::string_view Suffix = "#libSceSocialScreen#1#libSceSocialScreen#Function";
    static constexpr std::array<std::string_view, 7> Nids{
        "pI7oFSPP65A", // sceSocialScreenInitialize
        "OVNpYTRqN74", // sceSocialScreenTerminate
        "6Me4hYsy3Kc", // sceSocialScreenSetMode
        "VMM7wQBZoBk", // sceSocialScreenInitializeSeparateModeParameter
        "IEzqdjIueps", // sceSocialScreenConfigureSeparateMode
        "SvdXHHt2LLE", // sceSocialScreenOpenSeparateMode
        "vtZIn9HtYbs", // sceSocialScreenCloseSeparateMode
    };
    static bool IsNid(std::string_view nid) {
        for (const auto n : Nids)
            if (n == nid)
                return true;
        return false;
    }

    struct SeparateModeParameter {
        s32 mode;
        u8 reserved[28];
    };
    static_assert(sizeof(SeparateModeParameter) == 32);

    struct State {
        bool initialized{};
        s32 mode{};
        bool configured{};
        s32 separate_mode{};
        bool separate_open{};
    };

    explicit GuestSocialScreen(GuestCpu::GuestAddressSpace& space) : space(space) {}

    // Runs one call; the result is left in `result`. Returns why the call is refused instead.
    std::optional<std::string> Call(std::string_view nid, const std::array<u64, 6>& a,
                                    u32& result) {
        result = 0;
        // Guest memory is read and written outside the lock.
        if (nid == "VMM7wQBZoBk") {
            if (!a[0])
                return "sceSocialScreenInitializeSeparateModeParameter: null parameter";
            const SeparateModeParameter cleared{};
            if (!space.WriteData(GuestCpu::GuestAddress{a[0]},
                                 std::as_bytes(std::span{&cleared, 1})))
                return "sceSocialScreenInitializeSeparateModeParameter: parameter not writable";
            return std::nullopt;
        }
        SeparateModeParameter parameter{};
        if (nid == "IEzqdjIueps" &&
            (!a[0] || !space.ReadData(GuestCpu::GuestAddress{a[0]},
                                      std::as_writable_bytes(std::span{&parameter, 1}))))
            return "sceSocialScreenConfigureSeparateMode: parameter not readable";
        std::scoped_lock lock{mutex};
        if (nid == "pI7oFSPP65A") {
            if (state.initialized)
                return "sceSocialScreenInitialize: already initialized";
            state = State{};
            state.initialized = true;
            return std::nullopt;
        }
        if (!state.initialized)
            return "libSceSocialScreen: called before sceSocialScreenInitialize";
        if (nid == "OVNpYTRqN74") {
            state = State{};
            return std::nullopt;
        }
        if (nid == "6Me4hYsy3Kc") {
            state.mode = static_cast<s32>(a[0]);
            return std::nullopt;
        }
        if (nid == "IEzqdjIueps") {
            state.configured = true;
            state.separate_mode = parameter.mode;
            return std::nullopt;
        }
        if (nid == "SvdXHHt2LLE") {
            if (state.separate_open)
                return "sceSocialScreenOpenSeparateMode: already open";
            state.separate_open = true;
            return std::nullopt;
        }
        if (nid == "vtZIn9HtYbs") {
            if (!state.separate_open)
                return "sceSocialScreenCloseSeparateMode: not open";
            state.separate_open = false;
            return std::nullopt;
        }
        return "libSceSocialScreen: unknown function " + std::string(nid);
    }

    State Snapshot() {
        std::scoped_lock lock{mutex};
        return state;
    }

private:
    GuestCpu::GuestAddressSpace& space;
    std::mutex mutex;
    State state;
};

} // namespace Core::HostRuntime
