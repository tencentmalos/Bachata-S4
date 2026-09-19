// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cstdio>
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/vr_tracker/vr_tracker_play_area.h"

using namespace Libraries::VrTracker;

int main() {
    unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
    using Bytes = std::array<u8, 0x40>;
    OrbisVrTrackerPlayAreaWarningInfo info{};
    CHECK(PlayAreaWarningInfoNoProvider(false, nullptr) == ORBIS_VR_TRACKER_ERROR_NOT_INIT);
    CHECK(PlayAreaWarningInfoNoProvider(false, &info) == ORBIS_VR_TRACKER_ERROR_NOT_INIT);
    CHECK(PlayAreaWarningInfoNoProvider(true, nullptr) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
    for (u32 size : {0u, 0x3fu, 0x41u, 0xffffffffu}) {
        info.size = size;
        const auto before = std::bit_cast<Bytes>(info);
        CHECK(PlayAreaWarningInfoNoProvider(true, &info) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        CHECK(std::bit_cast<Bytes>(info) == before);
    }
    info.size = sizeof(info);
    const auto baseline = std::bit_cast<Bytes>(info);
    // Firmware validates every reserved byte. Check each independently,
    // including padding adjacent to the two byte-sized output flags.
    for (size_t offset = 4; offset < baseline.size(); ++offset) {
        const bool reserved = offset != 0x10 && offset != 0x20 &&
                              !(offset >= 0x24 && offset < 0x2c);
        auto bytes = baseline;
        bytes[offset] = 0xff;
        auto candidate = std::bit_cast<OrbisVrTrackerPlayAreaWarningInfo>(bytes);
        CHECK(PlayAreaWarningInfoNoProvider(true, &candidate) ==
              (reserved ? ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID :
                          ORBIS_VR_TRACKER_ERROR_NOT_SUPPORTED));
        CHECK(std::bit_cast<Bytes>(candidate) == bytes);
    }
    CHECK(PlayAreaWarningInfoNoProvider(true, &info) == ORBIS_VR_TRACKER_ERROR_NOT_SUPPORTED);
    CHECK(std::bit_cast<Bytes>(info) == baseline);
    std::printf("GUEST_VR_ABI checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
