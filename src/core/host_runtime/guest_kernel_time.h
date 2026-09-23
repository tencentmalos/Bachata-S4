// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <bit>
#include <cstring>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
inline constexpr std::array<std::string_view, 3> KernelTimezoneNids{
    "kOcnerypnQA", "-o5uEDpN+oY", "0NTHN1NKONI"};

// libkernel's fallback adds these signed minute offsets for UTC -> local.
// The current session uses UTC, consistently with gettimeofday and GuestRtc.
struct GuestKernelTimezone {
    s32 offset_minutes{};
    s32 dst_minutes{};
};
struct GuestKernelTimeInfo {
    u64 utc_seconds{};
    s32 offset_seconds{};
    s32 dst_seconds{};
};
static_assert(sizeof(GuestKernelTimezone) == 8);
static_assert(sizeof(GuestKernelTimeInfo) == 16);

inline u32 DispatchKernelTimezone(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                                 const std::array<u64, 6>& args,
                                 GuestKernelTimezone zone = {}) {
    using namespace GuestCpu;
    struct Output { u64 address; const void* data; size_t size; };
    std::array<Output, 3> outputs{};
    const bool to_utc = nid == "0NTHN1NKONI";
    // Unsigned arithmetic preserves the firmware's 64-bit wrapping add/sub;
    // the 32-bit metadata and DST output are also the exact stored ABI widths.
    const s64 delta = (s64(zone.offset_minutes) + zone.dst_minutes) * 60;
    const u64 converted = to_utc ? args[0] - u64(delta) : args[0] + u64(delta);
    const s32 dst = std::bit_cast<s32>(u32(zone.dst_minutes) * 60u);
    const GuestKernelTimeInfo info{
        to_utc ? converted : args[0],
        std::bit_cast<s32>(u32(zone.offset_minutes) * 60u), dst};
    if (nid == "kOcnerypnQA") {
        outputs[0] = {args[0], &zone, sizeof(zone)};
    } else if (nid == "-o5uEDpN+oY" || to_utc) {
        // Local -> UTC has an additional second scalar argument. For a fixed
        // zone there is no ambiguous DST transition to select, as in the
        // firmware fallback. Its fourth output is TimeInfo, NOT an 8B timezone.
        const size_t first = to_utc ? 2 : 1;
        outputs = {{{args[first], &converted, 8}, {args[first + 1], &info, 16},
                    {args[first + 2], &dst, 4}}};
    } else {
        return ORBIS_KERNEL_ERROR_ENOSYS;
    }
    std::array<GuestAddressSpace::DataRequest, 3> requests{};
    size_t count{};
    for (const auto& output : outputs) {
        if (!output.address) continue; // All outputs are independently optional.
        requests[count++] = {{{output.address}, output.size}, GuestPermission::Write};
    }
    auto pins = space.AcquireDataBatch(std::span{requests}.first(count));
    if (!pins) return ORBIS_KERNEL_ERROR_EFAULT;
    size_t index{};
    for (const auto& output : outputs)
        if (output.address)
            std::memcpy(pins.Value()[index++].WritableBytes().data(), output.data, output.size);
    return 0;
}
} // namespace Core::HostRuntime
