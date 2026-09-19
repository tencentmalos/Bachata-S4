// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <mutex>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/playgo/playgo.h"

namespace Core::HostRuntime {
inline constexpr std::string_view PlayGoNids[]{
    "Uco1I0dlDi8", "73fF1MFU8hA", "v6EZ-YWRdMs", "rvBSfTimejE", "3OMbYZBaa50",
    "uWIYLFkkwqk", "-RJWNMK3fC8", "Nn7zKwnA5q0", "ts6GlZOKRrE", "M1Gma1ocrGE",
    "-Q1-u1a7p0g", "4AAcTU9R3XM", "LosLlHOpNqQ", "gUPGiOQ1tmQ", "MPe0EeBGM-E"};
inline bool IsPlayGoNid(std::string_view nid) {
    return std::ranges::find(PlayGoNids, nid) != std::end(PlayGoNids);
}
// Uses the desktop's installed-content/chunk policy. Global desktop services are
// leased to one production Session; this domain serializes just PlayGo lifetime.
class GuestPlayGo {
    std::mutex mutex;
    bool initialized{};
public:
    ~GuestPlayGo() { if (initialized) Libraries::PlayGo::scePlayGoTerminate(); }
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a) {
        using namespace GuestCpu;
        using namespace Libraries::PlayGo;
        std::lock_guard lock(mutex);
        constexpr auto bad = u32(ORBIS_PLAYGO_ERROR_BAD_POINTER);
        if (nid == "ts6GlZOKRrE") {
            OrbisPlayGoInitParams p{};
            if (!space.ReadData(GuestAddress{a[0]}, std::as_writable_bytes(std::span{&p, 1})) ||
                !p.bufAddr) return bad;
            if (p.bufSize < 0x200000) return ORBIS_PLAYGO_ERROR_BAD_SIZE;
            if (!space.ValidateRange({GuestAddress{reinterpret_cast<u64>(p.bufAddr)}, p.bufSize},
                                     GuestPermission::Write)) return bad;
            // The shared implementation reads chunk metadata from disk. No guest
            // pin crosses that I/O; the validated scratch buffer is never used.
            const auto result = Initialize(p.bufSize);
            if (!result) initialized = true;
            return result;
        }
        if (nid == "MPe0EeBGM-E") {
            if (!initialized) return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
            const auto result = scePlayGoTerminate();
            if (!result) initialized = false;
            return result;
        }
        const bool opening = nid == "M1Gma1ocrGE";
        if (!opening && u32(a[0]) != 1) return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
        if (!initialized) return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
        // Remaining desktop calls neither retain pointers nor wait/do file I/O.
        // Checked leases let native queries preserve their partial-output rules.
        std::vector<GuestAddressSpace::DataRequest> requests;
        auto output = [&](u64 address, u64 size, bool write = true) {
            requests.push_back({{GuestAddress{address}, size},
                                write ? GuestPermission::Write : GuestPermission::Read});
        };
        if (opening) output(a[0], 4);
        else if (nid == "rvBSfTimejE") output(a[1], 4);
        else if (nid == "3OMbYZBaa50") output(a[1], 8);
        else if (nid != "Uco1I0dlDi8" && nid != "4AAcTU9R3XM" && nid != "LosLlHOpNqQ") {
            if (a[2] > 65536 || (!a[2] && !(nid == "73fF1MFU8hA" && !a[1])))
                return ORBIS_PLAYGO_ERROR_BAD_SIZE;
            if (nid == "73fF1MFU8hA") {
                if (a[1]) output(a[1], a[2] * 2);
                output(a[3], 4);
            } else if (nid == "Nn7zKwnA5q0") {
                output(a[1], a[2] * 4); output(a[3], 4);
            } else if (nid == "gUPGiOQ1tmQ") output(a[1], a[2] * 4, false);
            else {
                output(a[1], a[2] * 2, false);
                if (nid == "v6EZ-YWRdMs") output(a[3], 8);
                else if (nid == "uWIYLFkkwqk") output(a[3], a[2]);
                else if (nid == "-RJWNMK3fC8") output(a[3], 16);
            }
        }
        // Atomic admission of the whole call: never wait for a second range
        // while retaining the first one against a concurrent VM retirement.
        auto pins = space.AcquireDataBatch(requests);
        if (!pins) return bad;
        auto pin = [&]<class T>(u64 address, u64 count, bool write) -> T* {
            for (size_t i = 0; i < requests.size(); ++i) {
                if (requests[i].range.base.value != address ||
                    requests[i].range.size != count * sizeof(T) ||
                    requests[i].permission != (write ? GuestPermission::Write : GuestPermission::Read)) continue;
                auto* data = write ? pins.Value()[i].WritableBytes().data()
                                   : const_cast<std::byte*>(pins.Value()[i].Bytes().data());
                return reinterpret_cast<T*>(data);
            }
            return nullptr;
        };
        if (opening) {
            if (a[1]) return ORBIS_PLAYGO_ERROR_INVALID_ARGUMENT;
            auto* out = pin.template operator()<u32>(a[0], 1, true);
            return out ? u32(scePlayGoOpen(out, nullptr)) : bad;
        }
        const u32 handle = a[0];
        if (nid == "Uco1I0dlDi8") return scePlayGoClose(handle);
        if (nid == "4AAcTU9R3XM") return scePlayGoSetInstallSpeed(handle, OrbisPlayGoInstallSpeed(a[1]));
        if (nid == "LosLlHOpNqQ") return scePlayGoSetLanguageMask(handle, a[1]);
        if (nid == "rvBSfTimejE") {
            auto* out = pin.template operator()<OrbisPlayGoInstallSpeed>(a[1], 1, true);
            return out ? u32(scePlayGoGetInstallSpeed(handle, out)) : bad;
        }
        if (nid == "3OMbYZBaa50") {
            auto* out = pin.template operator()<u64>(a[1], 1, true);
            return out ? u32(scePlayGoGetLanguageMask(handle, out)) : bad;
        }
        if (a[2] > 65536) return ORBIS_PLAYGO_ERROR_BAD_SIZE;
        const u32 count = a[2];
        if (nid == "73fF1MFU8hA") {
            if (a[1] && !count) return ORBIS_PLAYGO_ERROR_BAD_SIZE;
            auto* out = pin.template operator()<u32>(a[3], 1, true);
            auto* ids = a[1] ? pin.template operator()<u16>(a[1], count, true) : nullptr;
            return out && (!a[1] || ids) ? u32(scePlayGoGetChunkId(handle, ids, count, out)) : bad;
        }
        if (!count) return ORBIS_PLAYGO_ERROR_BAD_SIZE;
        if (nid == "Nn7zKwnA5q0") {
            auto* list = pin.template operator()<OrbisPlayGoToDo>(a[1], count, true);
            auto* out = pin.template operator()<u32>(a[3], 1, true);
            return list && out ? u32(scePlayGoGetToDoList(handle, list, count, out)) : bad;
        }
        if (nid == "gUPGiOQ1tmQ") {
            auto* list = pin.template operator()<OrbisPlayGoToDo>(a[1], count, false);
            return list ? u32(scePlayGoSetToDoList(handle, list, count)) : bad;
        }
        auto* ids = pin.template operator()<u16>(a[1], count, false);
        if (!ids) return bad;
        if (nid == "-Q1-u1a7p0g") return scePlayGoPrefetch(handle, ids, count, OrbisPlayGoLocus(a[3]));
        if (nid == "v6EZ-YWRdMs") {
            auto* out = pin.template operator()<OrbisPlayGoEta>(a[3], 1, true);
            return out ? u32(scePlayGoGetEta(handle, ids, count, out)) : bad;
        }
        if (nid == "uWIYLFkkwqk") {
            auto* out = pin.template operator()<OrbisPlayGoLocus>(a[3], count, true);
            return out ? u32(scePlayGoGetLocus(handle, ids, count, out)) : bad;
        }
        if (nid == "-RJWNMK3fC8") {
            auto* out = pin.template operator()<OrbisPlayGoProgress>(a[3], 1, true);
            return out ? u32(scePlayGoGetProgress(handle, ids, count, out)) : bad;
        }
        return ORBIS_PLAYGO_ERROR_INVALID_ARGUMENT;
    }
};
} // namespace Core::HostRuntime
