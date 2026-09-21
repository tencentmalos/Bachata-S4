// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
inline constexpr std::array<std::string_view, 9> DirectMemoryServiceNids{
    "B+vc2AO2Zrc", "C0f7TJcbfac", "hwVSPCmp5tM", "qCSfqDILlns", "pU-QydtGcGY",
    "Vzl66WmfLvk", "LXo1tpFqJGs", "YN878uKRBbE", "bvD+95Q6asU"};

// Mapping calls may retire their output's mapping. Remember its identity, but
// never hold a data pin across a mapping writer (including batch operations).
struct MemoryOutput {
    GuestCpu::GuestRange range;
    std::vector<GuestCpu::GuestAddressSpace::MappingIdentity> identities;
    GuestCpu::GuestAddressSpace::DataRequest Request() const {
        return {range, GuestCpu::GuestPermission::Write, identities};
    }
};
inline GuestCpu::Result<MemoryOutput> MemoryOutputRequest(
    GuestCpu::GuestAddressSpace& space, u64 address, u64 size) {
    using namespace GuestCpu;
    MemoryOutput request{{{address}, size}, {}};
    auto pin = space.AcquireDataSpan(request.range, true);
    if (!pin) return pin.GetError();
    for (u64 cursor = address; cursor < request.range.End();) {
        auto mapping = space.Query({cursor});
        if (!mapping) return mapping.GetError();
        const auto end = std::min(request.range.End(), mapping.Value().range.End());
        request.identities.push_back({cursor, end, mapping.Value().mapping_generation});
        cursor = end;
    }
    return request;
}

inline u64 DispatchDirectMemoryService(GuestCpu::GuestAddressSpace& space,
                                      std::string_view nid, const std::array<u64, 6>& a) {
    using namespace GuestCpu;
    using namespace Libraries::Kernel;
    if (nid == "B+vc2AO2Zrc" || nid == "qCSfqDILlns") {
        const bool main = nid == "B+vc2AO2Zrc";
        auto pin = space.AcquireDataSpan({{a[main ? 3 : 4]}, 8}, true);
        if (!pin) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        u64 physical{};
        s64 direct{};
        const auto result = main
            ? sceKernelAllocateMainDirectMemory(a[0], a[1], s32(a[2]), &direct)
            : sceKernelMemoryPoolExpand(a[0], a[1], a[2], a[3], &physical);
        if (main) physical = u64(direct);
        if (!result) std::memcpy(pin.Value().WritableBytes().data(), &physical, 8);
        return u32(result);
    }
    if (nid == "C0f7TJcbfac") {
        const std::array<GuestAddressSpace::DataRequest, 2> requests{{
            {{{a[3]}, 8}, GuestPermission::Write}, {{{a[4]}, 8}, GuestPermission::Write}}};
        auto pins = space.AcquireDataBatch(requests);
        if (!pins) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        u64 physical{}, size{};
        const auto result = sceKernelAvailableDirectMemorySize(a[0], a[1], a[2], &physical, &size);
        if (!result) {
            std::memcpy(pins.Value()[0].WritableBytes().data(), &physical, 8);
            std::memcpy(pins.Value()[1].WritableBytes().data(), &size, 8);
        }
        return u32(result);
    }
    if (nid == "hwVSPCmp5tM") return u32(sceKernelCheckedReleaseDirectMemory(a[0], a[1]));
    if (nid == "pU-QydtGcGY") {
        auto output = MemoryOutputRequest(space, a[4], 8);
        if (!output) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        if (a[1] > UINT64_MAX - a[0] ||
            (a[0] < a[4] + 8 && a[4] < a[0] + a[1]))
            return u32(ORBIS_KERNEL_ERROR_EINVAL);
        void* address{};
        const auto result = sceKernelMemoryPoolReserve(reinterpret_cast<void*>(a[0]), a[1],
                                                       a[2], s32(a[3]), &address);
        if (result) return u32(result);
        const std::array requests{output.Value().Request()};
        auto pins = space.AcquireDataBatch(requests);
        if (!pins) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        const auto value = reinterpret_cast<u64>(address);
        std::memcpy(pins.Value()[0].WritableBytes().data(), &value, 8);
        return 0;
    }
    if (nid == "Vzl66WmfLvk")
        return u32(sceKernelMemoryPoolCommit(reinterpret_cast<void*>(a[0]), a[1], s32(a[2]),
                                             s32(a[3]), s32(a[4])));
    if (nid == "LXo1tpFqJGs")
        return u32(sceKernelMemoryPoolDecommit(reinterpret_cast<void*>(a[0]), a[1], s32(a[2])));
    if (nid == "bvD+95Q6asU") {
        OrbisKernelMemoryPoolBlockStats stats{};
        const auto size = std::min<u64>(a[1], sizeof(stats));
        if (!size) return 0;
        auto pin = space.AcquireDataSpan({{a[0]}, size}, true);
        if (!pin) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        const auto result = sceKernelMemoryPoolGetBlockStats(&stats, sizeof(stats));
        if (!result) std::memcpy(pin.Value().WritableBytes().data(), &stats, size);
        return u32(result);
    }
    if (nid == "YN878uKRBbE") {
        using Entry = OrbisKernelMemoryPoolBatchEntry;
        static_assert(sizeof(Entry) == 32 && offsetof(Entry, commit_params) == 8);
        if (!a[0] || s32(a[1]) < 0 || a[1] > 65536) return u32(ORBIS_KERNEL_ERROR_EINVAL);
        std::vector<Entry> entries(a[1]);
        if (!entries.empty() && !space.ReadData({a[0]}, std::as_writable_bytes(std::span(entries))))
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
        std::vector<MemoryOutput> outputs;
        if (a[2]) {
            auto request = MemoryOutputRequest(space, a[2], 4);
            if (!request) return u32(ORBIS_KERNEL_ERROR_EFAULT);
            outputs.push_back(std::move(request).Value());
        }
        s32 result{}, processed{};
        for (const auto& entry : entries) {
            const auto address = reinterpret_cast<u64>(entry.decommit_params.addr);
            const auto length = entry.decommit_params.len;
            // Protect/TypeProtect share the desktop path, which expects an owned
            // range. Move remains an explicit unsupported operation, not a no-op.
            if (entry.opcode == OrbisKernelMemoryPoolOpcode::Move) {
                result = ORBIS_KERNEL_ERROR_ENOSYS;
                break;
            }
            if (length > UINT64_MAX - address ||
                !space.OwnsRange({{address}, length}) ||
                (a[2] && address < a[2] + 4 && a[2] < address + length)) {
                result = ORBIS_KERNEL_ERROR_EINVAL;
                break;
            }
            result = sceKernelMemoryPoolBatch(&entry, 1, nullptr, s32(a[3]));
            if (result) break;
            ++processed;
        }
        if (!outputs.empty()) {
            const std::array requests{outputs.front().Request()};
            auto pins = space.AcquireDataBatch(requests);
            if (!pins) return u32(ORBIS_KERNEL_ERROR_EFAULT);
            std::memcpy(pins.Value()[0].WritableBytes().data(), &processed, 4);
        }
        return u32(result);
    }
    return u32(ORBIS_KERNEL_ERROR_ENOSYS);
}
} // namespace Core::HostRuntime
