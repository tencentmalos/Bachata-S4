// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/ime/ime_common.h"
#include "core/libraries/ime/ime_error.h"

namespace Core::HostRuntime {

// Desktop's physical-keyboard subscription, with no keyboard resource provider.
// Opening a subscription is distinct from connecting a USB keyboard. There are
// no keyboard events to deliver; software text entry remains GuestImeDialog.
class GuestImeKeyboard {
public:
    inline static constexpr std::array<std::string_view, 5> Nids{
        "eaFXjfJv3xs", "PMVehSlfZ94", "dKadqZFgKKQ", "VkqLPArfFdc", "-4GCfYdNF1s"};
    static bool IsNid(std::string_view nid) {
        return std::ranges::find(Nids, nid) != Nids.end();
    }
    static bool Admits(std::string_view nid, std::string_view suffix) {
        return IsNid(nid) && suffix == "#libSceIme#1#libSceIme#Function";
    }
    void Cancel() {
        std::lock_guard lock(mutex);
        stopped = true;
        opened = false;
        parameter = {};
    }
    u32 Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
               const std::array<u64, 6>& args) {
        std::lock_guard lock(mutex);
        if (stopped)
            return ORBIS_IME_ERROR_NOT_OPENED;
        if (nid == "eaFXjfJv3xs") {
            OrbisImeKeyboardParam value{};
            if (!args[1] || !space.ReadData({args[1]}, std::as_writable_bytes(std::span{&value, 1})))
                return ORBIS_IME_ERROR_INVALID_ADDRESS;
            const u64 callback = reinterpret_cast<u64>(value.handler);
            if (!callback || !space.ValidateRange({{callback}, 1}, GuestCpu::GuestPermission::Execute))
                return ORBIS_IME_ERROR_INVALID_HANDLER;
            if (u32(value.option) & ~kValidOrbisImeKeyboardOptionMask)
                return ORBIS_IME_ERROR_INVALID_OPTION;
            if (s32(args[0]) == -1)
                return ORBIS_IME_ERROR_INVALID_USER_ID;
            const auto zero = [](auto byte) { return byte == 0; };
            if (!std::ranges::all_of(value.reserved1, zero) ||
                !std::ranges::all_of(value.reserved2, zero))
                return ORBIS_IME_ERROR_INVALID_RESERVED;
            if (opened)
                return ORBIS_IME_ERROR_BUSY;
            parameter = value; // owned snapshot, opaque callback/arg are never native-called
            opened = true;
            return 0;
        }
        if (nid == "PMVehSlfZ94") {
            if (!opened)
                return ORBIS_IME_ERROR_NOT_OPENED;
            if (s32(args[0]) == -1)
                return ORBIS_IME_ERROR_INVALID_USER_ID;
            opened = false;
            parameter = {};
            return 0;
        }
        if (nid == "dKadqZFgKKQ") {
            if (!args[1])
                return ORBIS_IME_ERROR_INVALID_ADDRESS;
            if (s32(args[0]) == -1)
                return ORBIS_IME_ERROR_INVALID_USER_ID;
            auto output = space.AcquireDataSpan({{args[1]}, sizeof(OrbisImeKeyboardResourceIdArray)}, true);
            if (!output)
                return ORBIS_IME_ERROR_INVALID_ADDRESS;
            const OrbisImeKeyboardResourceIdArray value{s32(args[0]), {}};
            std::memcpy(output.Value().WritableBytes().data(), &value, sizeof(value));
            return opened ? ORBIS_IME_ERROR_CONNECTION_FAILED : ORBIS_IME_ERROR_NOT_OPENED;
        }
        if (nid == "VkqLPArfFdc") {
            if (!args[1] || !space.ValidateRange({{args[1]}, sizeof(OrbisImeKeyboardInfo)},
                                                GuestCpu::GuestPermission::Write))
                return ORBIS_IME_ERROR_INVALID_ADDRESS;
            // No resource IDs exist. Do not reuse desktop's zero-filled success
            // stub, which would claim a keyboard's device/layout/status exists.
            return ORBIS_IME_ERROR_NO_RESOURCE_ID;
        }
        if (nid == "-4GCfYdNF1s")
            return opened ? 0 : ORBIS_IME_ERROR_NOT_OPENED;
        return ORBIS_IME_ERROR_INVALID_PARAM;
    }

private:
    std::mutex mutex;
    bool opened{}, stopped{};
    OrbisImeKeyboardParam parameter{};
};
} // namespace Core::HostRuntime
