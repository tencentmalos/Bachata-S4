// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_platform.h"
#include "core/libraries/disc_map/disc_map_codes.h"

namespace Core::HostRuntime {
inline constexpr std::string_view DiscMapNids[]{"fl1eoDnwQ4s", "lbQKqsERhtE", "fJgP+wqifno",
                                                "ioKMruft1ek", "5+vOlukvkfg"};
inline bool IsDiscMapNid(std::string_view nid) {
    return std::find(std::begin(DiscMapNids), std::end(DiscMapNids), nid) != std::end(DiscMapNids);
}

// The production content is unpacked; as on desktop it has no optical-disc
// bitmap. This is a real negative capability, not a successful fabricated map.
inline u32 DispatchDiscMap(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                           const std::array<u64, 6>& args) {
    if (nid != "fJgP+wqifno")
        return ORBIS_DISC_MAP_ERROR_NO_BITMAP_INFO;
    // Desktop's flags query writes three zeros. Validate all outputs before the
    // first write, retaining no native pointer or guest pin after this call.
    auto flags = space.AcquirePinnedSpan({GuestCpu::GuestAddress{args[3]}, 4}, true);
    auto first = space.AcquirePinnedSpan({GuestCpu::GuestAddress{args[4]}, 4}, true);
    auto second = space.AcquirePinnedSpan({GuestCpu::GuestAddress{args[5]}, 4}, true);
    if (!flags || !first || !second)
        return u32(ORBIS_KERNEL_ERROR_EFAULT);
    std::memset(flags.Value().WritableBytes().data(), 0, 4);
    std::memset(first.Value().WritableBytes().data(), 0, 4);
    std::memset(second.Value().WritableBytes().data(), 0, 4);
    return 0;
}

// This operation acquires API references to initialized providers. It cannot
// load a new ELF or infer a provider from DT_NEEDED. The explicit optional
// desktop Json2 compatibility has bookkeeping only, without function bindings. Existing guest
// modules were started by the runtime dependency graph; host providers publish
// only after installing their actual checked import surface.
inline u32 LoadInitializedSysmodule(GuestSysmodules& modules, GuestCpu::GuestAddressSpace& space,
                                    const std::array<u64, 6>& args) {
    const s32 bytes = static_cast<s32>(args[1]);
    if (args[3])
        return u32(ORBIS_SYSMODULE_INVALID_ID);
    if (bytes < 0 || bytes > 1024 * 1024)
        return u32(ORBIS_KERNEL_ERROR_EINVAL);
    if (bytes && !space.ValidateRange({GuestCpu::GuestAddress{args[2]}, u64(bytes)},
                                      GuestCpu::GuestPermission::Read))
        return u32(ORBIS_KERNEL_ERROR_EFAULT);
    std::optional<GuestCpu::PinnedSpan> output;
    if (args[4]) {
        auto pin = space.AcquirePinnedSpan({GuestCpu::GuestAddress{args[4]}, 4}, true);
        if (!pin)
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
        output = std::move(pin).Value();
    }
    s32 result{};
    if (output)
        std::memcpy(&result, output->Bytes().data(), 4);
    const s32 status = modules.Load(u32(args[0]), output ? &result : nullptr);
    if (!status && output)
        std::memcpy(output->WritableBytes().data(), &result, 4);
    return u32(status);
}
} // namespace Core::HostRuntime
