// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/coredump/coredump_error.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/kernel.h"
#include "core/module.h"

namespace Core::HostRuntime {
// Enumerate the prepared Linker graph, preserving its handles. No host module or
// pointer is exposed. GetModuleList2 omits system libraries, as on desktop.
template <class Lookup>
u32 DispatchModuleList(GuestCpu::GuestAddressSpace& space, bool hide_system,
                       u64 handles, u64 capacity, u64 out_count, Lookup&& lookup) {
    if (!handles || !out_count) return ORBIS_KERNEL_ERROR_EFAULT;
    std::vector<s32> ids;
    bool truncated = false;
    for (s32 id = 0;; ++id) {
        const auto system = lookup(id);
        if (!system) break;
        if (hide_system && *system) continue;
        if (ids.size() == capacity) { truncated = true; break; }
        ids.push_back(id);
    }
    auto count = space.AcquireDataSpan({{out_count}, sizeof(u64)}, true);
    if (!count) return ORBIS_KERNEL_ERROR_EFAULT;
    if (!ids.empty()) {
        auto output = space.AcquireDataSpan({{handles}, ids.size() * sizeof(s32)}, true);
        if (!output) return ORBIS_KERNEL_ERROR_EFAULT;
        std::memcpy(output.Value().WritableBytes().data(), ids.data(), ids.size() * sizeof(s32));
    }
    // Desktop fills the available handle slots but leaves count untouched on ENOMEM.
    if (truncated) return ORBIS_KERNEL_ERROR_ENOMEM;
    const u64 size = ids.size();
    std::memcpy(count.Value().WritableBytes().data(), &size, sizeof(size));
    return 0;
}

template <class Info, class Lookup>
u32 DispatchModuleHandleInfo(GuestCpu::GuestAddressSpace& space, bool hide_system,
                             s32 handle, u64 output, Lookup&& lookup) {
    if (!output) return ORBIS_KERNEL_ERROR_EFAULT;
    u64 requested{};
    if (!space.ReadData({output}, std::as_writable_bytes(std::span{&requested, 1})))
        return ORBIS_KERNEL_ERROR_EFAULT;
    if (requested != sizeof(Info)) return ORBIS_KERNEL_ERROR_EINVAL;
    auto pin = space.AcquireDataSpan({{output}, sizeof(Info)}, true);
    if (!pin) return ORBIS_KERNEL_ERROR_EFAULT;
    const auto source = lookup(handle);
    if (!source) return ORBIS_KERNEL_ERROR_ESRCH;
    if (hide_system && source->second) return ORBIS_KERNEL_ERROR_EPERM;
    std::memcpy(pin.Value().WritableBytes().data(), &source->first, sizeof(Info));
    return 0;
}

// Generate into native storage, then publish the complete 16-byte result through
// a checked guest pin. No guest address is passed to the host UUID provider.
inline u32 DispatchKernelUuidCreate(GuestCpu::GuestAddressSpace& space, u64 address) {
    if (!address) return ORBIS_KERNEL_ERROR_EINVAL;
    auto output = space.AcquireDataSpan({{address}, sizeof(Libraries::Kernel::OrbisKernelUuid)}, true);
    if (!output) return ORBIS_KERNEL_ERROR_EFAULT;
    Libraries::Kernel::OrbisKernelUuid uuid{};
    const auto result = Libraries::Kernel::sceKernelUuidCreate(&uuid);
    if (result) return result;
    std::memcpy(output.Value().WritableBytes().data(), &uuid, sizeof(uuid));
    return 0;
}

inline constexpr std::array<std::string_view, 4> CoredumpUnavailableNids{
    "8zLSfEfW5AU", "fFkhOgztiCA", "Uxqkdta7wEg", "Dbbkj6YHWdo"};

// No coredump worker/provider exists in this runtime. Never retain a guest
// callback or claim registration succeeded. Writes are legal only from that
// worker, so no supplied guest pointer is dereferenced on this failure path.
inline u64 DispatchUnavailableCoredump(std::string_view nid, u64 handler, u64 stack_size) {
    if (nid == "8zLSfEfW5AU")
        return u32(!handler || stack_size < 0x4000 || stack_size >= 0x20000000
                       ? ORBIS_COREDUMP_ERROR_PARAM : ORBIS_COREDUMP_ERROR_THREAD_CREATE);
    if (nid == "fFkhOgztiCA") return u32(ORBIS_COREDUMP_ERROR_NOT_REGISTERED);
    // WriteUserData returns ssize_t; the guest tests the sign of all 64 bits.
    if (nid == "Dbbkj6YHWdo") return u64(s64(ORBIS_COREDUMP_ERROR_NOT_IN_COREDUMP_HANDLER));
    return u32(ORBIS_COREDUMP_ERROR_NOT_IN_COREDUMP_HANDLER);
}

// Firmware libkernel (11.00) distinguishes the sigreturn trampoline from a
// syscall return by instruction bytes, after checking RX permission. Keep all
// reads checked, including the backward read and truncated mapping boundaries.
inline u32 ClassifyGuestSignalReturn(GuestCpu::GuestAddressSpace& space, u64 pc) {
    using namespace GuestCpu;
    if (!space.ValidateRange({{pc}, 1}, GuestPermission::Read | GuestPermission::Execute)) return 0;
    constexpr std::array<u8, 19> signal{
        0x48,0x8d,0x7c,0x24,0x40,0x6a,0x00,0x48,0xc7,0xc0,
        0xa1,0x01,0x00,0x00,0x0f,0x05,0xf4,0xeb,0xfd};
    std::array<u8, 19> bytes{};
    if (space.ReadData({pc}, std::as_writable_bytes(std::span{bytes})) && bytes == signal) return 1;
    constexpr std::array<u8, 5> syscall{0x49,0x89,0xca,0x0f,0x05};
    std::array<u8, 5> before{};
    return pc >= 5 && space.ReadData({pc - 5}, std::as_writable_bytes(std::span{before})) && before == syscall ? 2 : 0;
}

// Both wrappers consume a snapshot of the same production Linker record. Pins
// cover validation and publication, with no guest/native pointer escape.
template <class Lookup, class HideName>
u32 DispatchModuleInfo(GuestCpu::GuestAddressSpace& space, bool unwind, bool sysmodule,
                       u64 address, s32 flags, u64 output, Lookup&& lookup, HideName&& hide_name) {
    using Info = Libraries::Kernel::OrbisModuleInfoForUnwind;
    const auto size = unwind ? sizeof(Info) : sizeof(OrbisKernelModuleInfoEx);
    if (!output) return ORBIS_KERNEL_ERROR_EFAULT;
    auto pin = space.AcquireDataSpan({{output}, size}, true);
    if (!pin) return ORBIS_KERNEL_ERROR_EFAULT;
    if (flags < 0 || flags >= 3) return ORBIS_KERNEL_ERROR_EINVAL;
    u64 requested{};
    if (unwind) {
        if (!space.ReadData({output}, std::as_writable_bytes(std::span{&requested, 1})))
            return ORBIS_KERNEL_ERROR_EFAULT;
        if (requested < sizeof(Info)) return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const auto source = lookup(address);
    if (!source) return ORBIS_KERNEL_ERROR_ESRCH;
    if (!unwind) {
        std::memcpy(pin.Value().WritableBytes().data(), &*source, sizeof(*source));
        return 0;
    }
    Info info{};
    info.st_size = requested;
    info.name = source->name;
    if (sysmodule && hide_name(info.name.data())) info.name.fill(0);
    info.eh_frame_hdr_addr = source->eh_frame_hdr_addr;
    info.eh_frame_addr = source->eh_frame_addr;
    info.eh_frame_size = source->eh_frame_size;
    if (source->segment_count) {
        info.seg0_addr = source->segments[0].address;
        info.seg0_size = source->segments[0].size;
    }
    std::memcpy(pin.Value().WritableBytes().data(), &info, sizeof(info));
    return 0;
}
} // namespace Core::HostRuntime
