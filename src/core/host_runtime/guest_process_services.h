// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/coredump/coredump_error.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/module.h"

namespace Core::HostRuntime {
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
