// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"

namespace Core::HostRuntime {
// Local SSL control domain only. The trust store is explicitly empty; no TLS
// connection or certificate validation API is admitted. Desktop Ssl/Ssl2 expose
// an empty list / a dummy non-certificate respectively, not a reusable engine.
class GuestSslOffline final {
public:
    static constexpr std::array<std::string_view, 5> Nids{
        "hdpVEUDFW3s", "0K1yQ6Lv-Yc", "TDfQqO-gMbY", "qIvLs0gYxi0", "-PoIzr3PEk0"};
    static constexpr u32 InvalidId = 0x8095f006, InvalidArgument = 0x8095f007, TooMany = 0x8095f008,
                         NotFound = 0x8095f009, NoMemory = 0x809517d5;
    struct CaCerts {
        u64 certs, count, pool;
    };
    struct Stats {
        u64 pool_size, max_inuse_size, current_inuse_size;
    };
    static_assert(sizeof(CaCerts) == 24 && sizeof(Stats) == 24);
    static bool IsNid(std::string_view nid) {
        return std::ranges::find(Nids, nid) != Nids.end();
    }
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& args) {
        std::lock_guard lock(mutex);
        if (nid == "hdpVEUDFW3s") {
            const auto size = args[0];
            if (size < sizeof(Header))
                return InvalidArgument;
            if (size > MaxPool || size > MaxReserved - reserved)
                return NoMemory;
            size_t index = next;
            for (size_t n = 0; n < slots.size(); ++n, index = (index + 1) % slots.size()) {
                auto& slot = slots[index];
                if (slot.bytes)
                    continue;
                auto memory = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[size]);
                if (!memory)
                    return NoMemory;
                // The header is the only allocation from an offline pool. Its
                // accounting is real and excludes host bookkeeping outside it.
                const Header header{0x5348414453534cULL, size};
                std::memcpy(memory.get(), &header, sizeof(header));
                slot = {std::move(memory), size, true};
                reserved += size;
                next = (index + 1) % slots.size();
                return u32(index + 1);
            }
            return TooMany;
        }
        const auto id = u32(args[0]);
        if (!id || id > slots.size())
            return InvalidId;
        auto& slot = slots[id - 1];
        if (!slot.used)
            return NotFound;
        if (!slot.bytes)
            return InvalidId;
        if (nid == "0K1yQ6Lv-Yc") {
            reserved -= slot.size;
            slot.bytes.reset();
            slot.size = 0;
            return 0;
        }
        if (!args[1])
            return InvalidArgument;
        if (nid == "qIvLs0gYxi0") {
            CaCerts certs{};
            if (!space.ReadData(GuestCpu::GuestAddress{args[1]},
                                std::as_writable_bytes(std::span{&certs, 1})))
                return InvalidArgument;
            // No foreign/guest pointers are ever passed to free(). Only this
            // backend's allocation-free empty list may be released.
            return certs.certs || certs.count || certs.pool ? InvalidArgument : 0;
        }
        if (nid == "TDfQqO-gMbY") {
            const CaCerts certs{};
            return space.WriteData(GuestCpu::GuestAddress{args[1]},
                                   std::as_bytes(std::span{&certs, 1}))
                       ? 0
                       : InvalidArgument;
        }
        if (nid == "-PoIzr3PEk0") {
            const Stats stats{slot.size, sizeof(Header), sizeof(Header)};
            return space.WriteData(GuestCpu::GuestAddress{args[1]},
                                   std::as_bytes(std::span{&stats, 1}))
                       ? 0
                       : InvalidArgument;
        }
        return InvalidArgument;
    }

private:
    struct Header {
        u64 magic, size;
    };
    struct Slot {
        std::unique_ptr<std::byte[]> bytes;
        u64 size{};
        bool used{};
    };
    // Emulator resource ceilings, not claimed as retail allocator limits.
    static constexpr u64 MaxPool = 32ULL << 20, MaxReserved = 64ULL << 20;
    std::mutex mutex;
    std::array<Slot, 128> slots;
    size_t next{};
    u64 reserved{};
};
} // namespace Core::HostRuntime
