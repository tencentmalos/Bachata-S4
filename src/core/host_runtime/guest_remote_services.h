// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include "common/types.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
// These clients require ShellCore/IPMI services which the Android host does
// not provide. Firmware propagates initialization transport errors, and checks
// initialization before touching the output pointers in the admitted methods.
// ENOSYS is our explicit provider boundary, not a claimed firmware IPC result.
// No initialized client, guest heap, connection or successful operation is
// fabricated. Initialization checks only its documented scalar size gate;
// optional workspaces remain untouched because no provider is instantiated.
struct RemoteServiceEntry {
    std::string_view nid, suffix;
    u32 error_base;
    bool initialize;
};
inline constexpr std::string_view SharePlaySuffix = "#libSceSharePlay#1#libSceSharePlay#Function";
inline constexpr std::string_view RemoteplaySuffix = "#libSceRemoteplay#1#libSceRemoteplay#Function";
inline constexpr std::array<RemoteServiceEntry, 9> RemoteServiceEntries{{
    {"isruqthpYcw", SharePlaySuffix, 0x810e0000, true}, // Initialize(void*, size_t)
    {"UaLjloJinow", SharePlaySuffix, 0x810e0000, false}, // Terminate
    {"co2NCj--pnc", SharePlaySuffix, 0x810e0000, false}, // SetProhibition
    {"+MCXJlWdi+s", SharePlaySuffix, 0x810e0000, false}, // GetCurrentConnectionInfoA
    {"k1SwgkMSOM8", RemoteplaySuffix, 0x80fc0000, true}, // Initialize(void*, size_t)
    {"BOwybKVa3Do", RemoteplaySuffix, 0x80fc0000, false}, // Terminate
    {"xQeIryTX7dY", RemoteplaySuffix, 0x80fc0000, false}, // Approve
    {"mrNh78tBpmg", RemoteplaySuffix, 0x80fc0000, false}, // Prohibit
    {"g3PNjYKWqnQ", RemoteplaySuffix, 0x80fc0000, false}, // GetConnectionStatus
}};
inline const RemoteServiceEntry* FindRemoteService(std::string_view nid) {
    for (const auto& entry : RemoteServiceEntries)
        if (nid == entry.nid) return &entry;
    return nullptr;
}
inline bool AdmitsRemoteService(std::string_view nid, std::string_view suffix) {
    const auto* entry = FindRemoteService(nid);
    return entry && entry->suffix == suffix;
}
inline u32 DispatchRemoteService(const RemoteServiceEntry& entry, const std::array<u64, 6>& a) {
    if (!entry.initialize) return entry.error_base | 4; // NOT_INITIALIZED
    // A null workspace asks the system client to allocate its own 0x1800 bytes.
    if (a[0] && a[1] < 0x1800) return entry.error_base | 1;
    return u32(ORBIS_KERNEL_ERROR_ENOSYS);
}
} // namespace Core::HostRuntime
