// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/app_info.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
inline u32 GuestGetAppInfo(GuestCpu::GuestAddressSpace& space, s32 pid, u64 out,
                           std::string_view title) {
    if (pid != 0xbad1)
        return ORBIS_KERNEL_ERROR_EPERM;
    if (!out) // Same optional output as the desktop implementation.
        return 0;
    Libraries::Kernel::OrbisKernelAppInfo info{};
    info.has_param_sfo = 1;
    std::memcpy(info.cusa_name, title.data(), std::min(title.size(), sizeof(info.cusa_name) - 1));
    return space.WriteData({out}, std::as_bytes(std::span{&info, 1}))
               ? 0 : u32(ORBIS_KERNEL_ERROR_EFAULT);
}

inline u32 GuestTitleWorkaround(GuestCpu::GuestAddressSpace& space, u64 input, s32 bit, u64 out) {
    if (!input || !out)
        return ORBIS_KERNEL_ERROR_EFAULT;
    if (bit < 0 || bit >= 0x3a)
        return ORBIS_KERNEL_ERROR_EINVAL;
    Libraries::Kernel::OrbisKernelTitleWorkaround value{};
    if (!space.ReadData({input}, std::as_writable_bytes(std::span{&value, 1})))
        return ORBIS_KERNEL_ERROR_EFAULT;
    const s32 result = (value.ids[bit >> 6] >> (bit & 0x3f)) & 1;
    return space.WriteData({out}, std::as_bytes(std::span{&result, 1}))
               ? 0 : u32(ORBIS_KERNEL_ERROR_EFAULT);
}
} // namespace Core::HostRuntime
