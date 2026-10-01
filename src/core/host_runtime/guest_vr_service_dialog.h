// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/system/commondialog.h"

namespace Core::HostRuntime {
// The PS4 system VR service dialog has no host provider. In particular, merely
// polling it before Initialize must return NONE, not fault the guest. Successful
// initialization/open/result are not fabricated, including during OpenXR use.
class GuestVrServiceDialog {
public:
    static constexpr std::array<std::string_view, 7> Nids{
        "60-cjn5Dn0Q", "M4xKWUytNMo", "RmRtBJpoHlA", "cYnBkgm8I0c",
        "hBH2ABP7IeY", "hYFXG8FWThI", "kUavKmsczkY"};
    static constexpr std::string_view Suffix =
        "#libSceVrServiceDialog#1#libSceVrServiceDialog#Function";
    static bool IsNid(std::string_view nid) {
        return std::ranges::find(Nids, nid) != Nids.end();
    }
    static u32 Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                      const std::array<u64, 6>& args) {
        using Error = Libraries::CommonDialog::Error;
        if (nid == "RmRtBJpoHlA" || nid == "kUavKmsczkY")
            return u32(Libraries::CommonDialog::Status::NONE);
        if (nid == "hYFXG8FWThI")
            return u32(Error::NOT_SUPPORTED); // Explicit host provider boundary.
        if (nid == "hBH2ABP7IeY" || nid == "M4xKWUytNMo")
            return u32(Error::NOT_INITIALIZED);
        // Firmware 11.00 validates Open/GetResult input before testing the
        // client pointer. Preserve that order and leave guest memory untouched.
        if (nid == "60-cjn5Dn0Q") {
            struct Param {
                Libraries::CommonDialog::BaseParam base;
                u64 size;
                u32 mode;
                u8 option;
                std::array<u8, 43> reserved;
            } param{};
            static_assert(sizeof(Param) == 0x68);
            if (!args[0] ||
                !space.ReadData({args[0]}, std::as_writable_bytes(std::span{&param, 1})))
                return u32(Error::ARG_NULL);
            if (param.base.size != sizeof(param.base) ||
                param.base.magic != u32(args[0] + 0xc0d1a109) ||
                !std::ranges::all_of(param.base.reserved, [](u8 v) { return v == 0; }) ||
                param.size != sizeof(param) || param.mode > 2 ||
                !std::ranges::all_of(param.reserved, [](u8 v) { return v == 0; }))
                return u32(Error::PARAM_INVALID);
            return u32(Error::NOT_INITIALIZED);
        }
        if (nid == "cYnBkgm8I0c") {
            std::array<u8, 0x24> result{};
            if (!args[0] || !space.ReadData({args[0]}, std::as_writable_bytes(std::span{result})))
                return u32(Error::ARG_NULL);
            if (!std::all_of(result.begin() + 4, result.end(), [](u8 v) { return v == 0; }))
                return u32(Error::PARAM_INVALID);
            return u32(Error::NOT_INITIALIZED);
        }
        return u32(Error::NOT_SUPPORTED);
    }
};
} // namespace Core::HostRuntime
