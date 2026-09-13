// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string_view>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
struct GuestThreadAttributes {
    u64 stack{}, size{1 << 20}, guard{};
    u64 affinity{}; // Zero means unrestricted; guest CPU IDs, not native cpu_set_t.
    s32 policy{1}, priority{700}, inherit{4}, scope{2}, detached{};
};
enum class ThreadAttrOp {
    Init,
    Destroy,
    GetThread,
    Stack,
    StackAddress,
    StackSize,
    Guard,
    Detach,
    Inherit,
    Policy,
    Priority,
    Scope,
    Affinity,
    AffinityMask,
    Suspend,
};
struct ThreadAttrEntry {
    std::string_view nid;
    ThreadAttrOp op;
    bool get{}, sce{};
};
inline constexpr ThreadAttrEntry ThreadAttrEntries[]{
#define ATTR(p, s, op, get) {p, ThreadAttrOp::op, get, false}, {s, ThreadAttrOp::op, get, true}
    ATTR("wtkt-teR1so", "nsYoNRywwNg", Init, false),
    ATTR("zHchY8ft5pk", "62KCwEMmzcM", Destroy, false),
    ATTR("Ucsu-OK+els", "x1X76arYMxU", GetThread, true),
    ATTR("vQm4fDEsWi8", "-quPa4SEJUw", Stack, true),
    ATTR("-SrbXpGR1f0", "Bvn74vj6oLo", Stack, false),
    ATTR("DxmIMUQ-wXY", "Ru36fiTtJzA", StackAddress, true),
    ATTR("suCrEbr0xIQ", "F+yfmduIBB8", StackAddress, false),
    ATTR("0qOtCR-ZHck", "-fA+7ZlGDQs", StackSize, true),
    ATTR("2Q0z6rnBrTE", "UTXzJbWhhTE", StackSize, false),
    ATTR("JNkVVsVDmOk", "txHtngJ+eyc", Guard, true),
    ATTR("JKyG3SWyA10", "El+cQ20DynU", Guard, false),
    ATTR("VUT1ZSrHT0I", "JaRMy+QcpeU", Detach, true),
    ATTR("E+tyo3lp5Lw", "-Wreprtu0Qs", Detach, false),
    ATTR("oLjPqUKhzes", "lpMP8HhkBbg", Inherit, true),
    ATTR("7ZlAakEf0Qg", "eXbUSpEaTsA", Inherit, false),
    ATTR("RtLRV-pBTTY", "NMyIQ9WgWbU", Policy, true),
    ATTR("JarMIy8kKEY", "4+h9EzwKF4I", Policy, false),
    ATTR("qlk9pSLsUmM", "FXPWHNk8Of0", Priority, true),
    ATTR("euKRgm0Vn2M", "DzES9hQF4f4", Priority, false),
    ATTR("e2G+cdEkOmU", "+7B2AEKKns8", Scope, true),
    ATTR("xesmlSI-KCI", "YdZfEZfRnPk", Scope, false),
    ATTR("Q2y5IqSDZGs", "GZSR0Ooae9Q", Suspend, false),
#undef ATTR
    {"-wzZ7dvA7UU", ThreadAttrOp::Affinity, true},
    {"o8pd4juNbgc", ThreadAttrOp::Affinity, false},
    {"8+s5BzZjxSg", ThreadAttrOp::AffinityMask, true, true},
    {"3qxgM4ezETA", ThreadAttrOp::AffinityMask, false, true},
};
class GuestThreadAttributeDomain final {
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;
    std::recursive_mutex* vm_mutex{};
    std::mutex mutex;
    std::map<u64, GuestThreadAttributes> objects;
    size_t allocations{};
    template <class T>
    bool Read(u64 address, T& value) {
        return bool(space.Read(GuestCpu::GuestAddress{address},
                               std::as_writable_bytes(std::span{&value, 1})));
    }
    template <class T>
    int Write(u64 address, const T& value) {
        return space.Write(GuestCpu::GuestAddress{address}, std::as_bytes(std::span{&value, 1}))
                   ? 0
                   : POSIX_EFAULT;
    }

public:
    static constexpr u64 MaxStack = 64ULL << 20;
    GuestThreadAttributeDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate,
                               std::recursive_mutex* vm_mutex = nullptr)
        : space(space), allocate(std::move(allocate)), vm_mutex(vm_mutex) {}
    static bool ValidSize(u64 size) {
        return size >= 0x4000 && size <= MaxStack;
    }
    static bool ValidPriority(s32 policy, s32 priority) {
        return policy == 2 ? priority >= 0x300 && priority <= 0x3bf
                           : (policy == 1 || policy == 3) && priority >= 0x100 && priority <= 0x2ff;
    }
    int Snapshot(u64 slot, GuestThreadAttributes& out) {
        std::lock_guard lock(mutex);
        if (!slot) {
            out = {};
            return 0;
        }
        u64 handle{};
        if (!Read(slot, handle))
            return POSIX_EFAULT;
        const auto it = objects.find(handle);
        if (it == objects.end())
            return POSIX_EINVAL;
        out = it->second; // No pointers into the mutable attribute domain survive create.
        return 0;
    }
    int Store(u64 slot, const GuestThreadAttributes& value) {
        std::lock_guard lock(mutex);
        u64 handle{};
        if (!Read(slot, handle))
            return POSIX_EFAULT;
        auto it = objects.find(handle);
        if (it == objects.end())
            return POSIX_EINVAL;
        it->second = value;
        return 0;
    }
    int Invoke(const ThreadAttrEntry& entry, u64 slot, u64 a, u64 b) {
        std::lock_guard lock(mutex);
        // No guest wait/callback in an attribute operation; pins stay local.
        std::unique_lock<std::recursive_mutex> vm;
        if (vm_mutex) vm = std::unique_lock(*vm_mutex);
        u64 handle{};
        if (!Read(slot, handle))
            return POSIX_EFAULT;
        auto it = objects.find(handle);
        if (entry.op == ThreadAttrOp::Init) {
            if (it != objects.end())
                return POSIX_EBUSY;
            if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                     GuestCpu::GuestPermission::Write))
                return POSIX_EFAULT;
            if (allocations >= 4096)
                return POSIX_ENOMEM;
            const u64 address = allocate(); // VM transaction precedes output pin.
            ++allocations;
            auto pin = space.AcquirePinnedSpan({GuestCpu::GuestAddress{slot}, 8}, true);
            if (!pin)
                return POSIX_EFAULT;
            objects.emplace(address, GuestThreadAttributes{});
            std::memcpy(pin.Value().WritableBytes().data(), &address, 8);
            return 0;
        }
        if (it == objects.end())
            return POSIX_EINVAL;
        auto& value = it->second;
        if (entry.op == ThreadAttrOp::Destroy) {
            if (int error = Write(slot, u64{}))
                return error;
            objects.erase(it);
            return 0;
        }
        if (entry.op == ThreadAttrOp::Suspend)
            return POSIX_ENOTSUP; // No resume gate yet.
        if (entry.op == ThreadAttrOp::Stack) {
            if (entry.get) {
                auto address = space.AcquirePinnedSpan({GuestCpu::GuestAddress{a}, 8}, true);
                auto size = space.AcquirePinnedSpan({GuestCpu::GuestAddress{b}, 8}, true);
                if (!address || !size)
                    return POSIX_EFAULT;
                std::memcpy(address.Value().WritableBytes().data(), &value.stack, 8);
                std::memcpy(size.Value().WritableBytes().data(), &value.size, 8);
            } else {
                if (!a || a % 16 || !ValidSize(b) || a > UINT64_MAX - b)
                    return POSIX_EINVAL;
                value.stack = a;
                value.size = b; // Mappings checked again by create.
            }
            return 0;
        }
        if (entry.op == ThreadAttrOp::Affinity || entry.op == ThreadAttrOp::AffinityMask) {
            if (entry.get) {
                const u64 mask = value.affinity ? value.affinity : 0xff;
                if (entry.op == ThreadAttrOp::AffinityMask)
                    return Write(a, mask);
                if (a < 8 || a > 128)
                    return POSIX_EINVAL;
                auto pin = space.AcquirePinnedSpan({GuestCpu::GuestAddress{b}, a}, true);
                if (!pin)
                    return POSIX_EFAULT;
                std::fill(pin.Value().WritableBytes().begin(), pin.Value().WritableBytes().end(),
                          std::byte{});
                std::memcpy(pin.Value().WritableBytes().data(), &mask, 8);
            } else {
                u64 mask = a;
                if (entry.op == ThreadAttrOp::Affinity) {
                    if (!a || !b) {
                        value.affinity = 0;
                        return 0;
                    }
                    if (a < 8 || a > 128)
                        return POSIX_EINVAL;
                    std::array<u8, 128> bytes{};
                    if (!space.Read(GuestCpu::GuestAddress{b},
                                    std::as_writable_bytes(std::span{bytes}).first(a)))
                        return POSIX_EFAULT;
                    std::memcpy(&mask, bytes.data(), 8);
                    if (std::any_of(bytes.begin() + 8, bytes.end(), [](u8 v) { return v; }))
                        return POSIX_EINVAL;
                }
                if (!mask || (mask & ~u64{0xff}))
                    return POSIX_EINVAL;
                value.affinity = mask;
            }
            return 0;
        }
        u64* wide{};
        s32* narrow{};
        switch (entry.op) {
        case ThreadAttrOp::StackAddress:
            wide = &value.stack;
            break;
        case ThreadAttrOp::StackSize:
            wide = &value.size;
            break;
        case ThreadAttrOp::Guard:
            wide = &value.guard;
            break;
        case ThreadAttrOp::Detach:
            narrow = &value.detached;
            break;
        case ThreadAttrOp::Inherit:
            narrow = &value.inherit;
            break;
        case ThreadAttrOp::Policy:
            narrow = &value.policy;
            break;
        case ThreadAttrOp::Priority:
            narrow = &value.priority;
            break;
        case ThreadAttrOp::Scope:
            narrow = &value.scope;
            break;
        default:
            return POSIX_ENOTSUP;
        }
        if (entry.get)
            return wide ? Write(a, *wide) : Write(a, *narrow);
        if (entry.op == ThreadAttrOp::Priority) {
            s32 priority{};
            if (!Read(a, priority))
                return POSIX_EFAULT;
            if (!ValidPriority(value.policy, priority))
                return POSIX_ENOTSUP;
            value.priority = priority;
            return 0;
        }
        if ((entry.op == ThreadAttrOp::StackAddress && (!a || a % 16)) ||
            (entry.op == ThreadAttrOp::StackSize && !ValidSize(a)) ||
            (entry.op == ThreadAttrOp::Guard && a > MaxStack) ||
            (entry.op == ThreadAttrOp::Detach && a > 1) ||
            (entry.op == ThreadAttrOp::Scope && a != 0 && a != 2))
            return POSIX_EINVAL;
        if (entry.op == ThreadAttrOp::Inherit && a != 0 && a != 4)
            return POSIX_ENOTSUP;
        if (entry.op == ThreadAttrOp::Policy) {
            if (a < 1 || a > 3)
                return POSIX_ENOTSUP;
            value.priority = a == 2 ? 0x384 : 0x2bc;
        }
        if (wide)
            *wide = a;
        else
            *narrow = static_cast<s32>(a);
        return 0;
    }
};
} // namespace Core::HostRuntime
