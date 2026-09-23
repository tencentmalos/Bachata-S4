// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/kernel.h"
#include "core/memory.h"
#include "core/host_runtime/guest_storage.h"

namespace Core::HostRuntime {

inline constexpr std::array<std::string_view, 6> MemoryServiceNids{
    "DGMG3JshrZU", "BHouLQzh0X0", "BPE9s9vQQXo", "UqDGjXA5yUM", "3k6kx-zOOSQ", "yDBwVAolDgg"};

inline u32 GuestQueryStack(GuestCpu::GuestAddressSpace& space, MemoryManager& memory,
                           u64 address, u64 start, u64 end) {
    using namespace GuestCpu;
    std::vector<GuestAddressSpace::DataRequest> requests;
    for (const u64 out : {start, end})
        if (out) requests.push_back({{{out}, 8}, GuestPermission::Write});
    auto pins = space.AcquireDataBatch(requests);
    if (!pins) return u32(ORBIS_KERNEL_ERROR_EFAULT);
    void *first{}, *last{};
    const auto result = memory.IsStack(address, &first, &last);
    if (!result) {
        size_t i = 0;
        if (start) std::memcpy(pins.Value()[i++].WritableBytes().data(), &first, 8);
        if (end) std::memcpy(pins.Value()[i].WritableBytes().data(), &last, 8);
    }
    return u32(result);
}

inline std::string_view MemoryServiceStatus(std::string_view nid) {
    if (nid == "3k6kx-zOOSQ") return "desktop_stub_no_residency";
    if (nid == "BPE9s9vQQXo") return "android_bridge_guest_file_mapping";
    return "android_bridge";
}

// Keep guest addresses/errno inside the session. The desktop MemoryManager
// supplies the actual VMA names, physical allocation queries and mapping rules.
inline GuestCpu::Result<u64> DispatchMemoryService(
    GuestCpu::GuestAddressSpace& space, MemoryManager& memory, GuestStorage& storage,
    std::string_view nid,
    const std::array<u64, 6>& a, const std::function<u64(int)>& posix_failure) {
    using namespace GuestCpu;
    if (nid == "yDBwVAolDgg")
        return u64(GuestQueryStack(space, memory, a[0], a[1], a[2]));
    if (nid == "DGMG3JshrZU") {
        std::string name;
        for (u64 i = 0; i < Libraries::Kernel::ORBIS_KERNEL_MAXIMUM_NAME_LENGTH; ++i) {
            char ch{};
            if (!a[2] || a[2] > UINT64_MAX - i ||
                !space.ReadData({a[2] + i}, std::as_writable_bytes(std::span{&ch, 1})))
                return u64(u32(ORBIS_KERNEL_ERROR_EFAULT));
            if (!ch)
                return u64(u32(memory.NameVirtualRange(a[0], a[1], name)));
            name.push_back(ch);
        }
        return u64(u32(ORBIS_KERNEL_ERROR_ENAMETOOLONG));
    }
    if (nid == "BHouLQzh0X0") {
        using Info = Libraries::Kernel::OrbisQueryInfo;
        static_assert(sizeof(Info) == 0x18 && offsetof(Info, memoryType) == 0x10);
        // The desktop function does not check infoSize. Never let a short guest
        // record receive the full native result; larger versioned records are OK.
        if (a[3] < sizeof(Info)) return u64(u32(ORBIS_KERNEL_ERROR_EINVAL));
        auto pin = space.AcquireDataSpan({{a[2]}, sizeof(Info)}, true);
        if (!pin) return u64(u32(ORBIS_KERNEL_ERROR_EFAULT));
        Info info{};
        const auto result = memory.DirectMemoryQuery(a[0], s32(a[1]) == 1, &info);
        if (!result) std::memcpy(pin.Value().WritableBytes().data(), &info, sizeof(info));
        return u64(u32(result));
    }
    if (nid == "BPE9s9vQQXo") {
        void* out{};
        const auto mapping = storage.AcquireMappingFile(s32(a[4]));
        if (mapping.error)
            return posix_failure(Libraries::Kernel::NativeToPosixErrno(mapping.error));
        const Core::NativeFileMapping native{static_cast<uintptr_t>(mapping.host_fd),
                                             mapping.writable, mapping.backend};
        const s32 result = Libraries::Kernel::MapVirtualMemory(
            reinterpret_cast<void*>(a[0]), a[1], s32(a[2]), s32(a[3]), s32(a[4]), s64(a[5]), &out,
            &native);
        return result ? posix_failure(u32(result) & 0xffff) : reinterpret_cast<u64>(out);
    }
    if (nid == "UqDGjXA5yUM") {
        const s32 result = Libraries::Kernel::sceKernelMunmap(reinterpret_cast<void*>(a[0]), a[1]);
        return result ? posix_failure(u32(result) & 0xffff) : u64{0};
    }
    if (nid == "3k6kx-zOOSQ") {
        // Deliberate desktop compatibility stub. This does NOT pin physical
        // memory, alter permissions or promise host residency. Audit it as such.
        return u64(u32(Libraries::Kernel::sceKernelMlock(reinterpret_cast<void*>(a[0]), a[1])));
    }
    return MakeError(ErrorCategory::Unsupported, "GuestMemory", "unknown memory service");
}
} // namespace Core::HostRuntime
