// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/http_error.h"
namespace Core::HostRuntime {
inline constexpr std::string_view HttpUriNids[]{"IWalAn-guFs", "5LZA+KPISVA", "YuOW3dDAKYc"};
inline bool IsHttpUriNid(std::string_view nid) {
    return std::ranges::find(HttpUriNids, nid) != std::end(HttpUriNids);
}
inline std::optional<std::string> HttpGuestText(GuestCpu::GuestAddressSpace& space, u64 at,
                                             size_t limit = 8192) {
    if (!at || at > UINT64_MAX-limit) return {};
    std::string value;
    for (size_t i=0; i<limit; ++i) {
        char ch{};
        if (!space.ReadData({at+i}, std::as_writable_bytes(std::span{&ch,1}))) return {};
        if (!ch) return value;
        value += ch;
    }
    return {};
}
// Pure URI helpers reuse the desktop parser. Every embedded pointer is rebased
// explicitly between the caller's guest pool and temporary host strings.
inline u32 DispatchHttpUri(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                          const std::array<u64,6>& a) {
    using namespace GuestCpu;
    using namespace Libraries::Http;
    constexpr u32 bad = ORBIS_HTTP_ERROR_INVALID_VALUE;
    struct Element { u8 opaque; u8 pad[7]; std::array<u64,7> strings; u16 port; u8 reserved[10]; } value{};
    static_assert(sizeof(Element) == 80 && sizeof(Element) == sizeof(OrbisHttpUriElement));
    OrbisHttpUriElement native{};
    const std::array<char* OrbisHttpUriElement::*,7> members{
        &OrbisHttpUriElement::scheme, &OrbisHttpUriElement::username,
        &OrbisHttpUriElement::password, &OrbisHttpUriElement::hostname,
        &OrbisHttpUriElement::path, &OrbisHttpUriElement::query, &OrbisHttpUriElement::fragment};
    const bool parse = nid == "IWalAn-guFs";
    const u64 out = a[0], required = a[parse ? 3 : 1], capacity = a[parse ? 4 : 2];
    const bool writing = parse ? out && a[2] : out != 0;
    if (writing && capacity > (1<<20)) return u32(ORBIS_HTTP_ERROR_OUT_OF_SIZE);
    if (parse && !writing && !required) return bad;
    if (nid == "5LZA+KPISVA" && !out && !required) return bad;
    std::vector<char> buffer(writing ? std::max<u64>(1,capacity) : 1);
    u64 need{};
    int result;
    if (parse) {
        auto text = HttpGuestText(space,a[1]);
        if (!text) return u32(ORBIS_HTTP_ERROR_INVALID_URL);
        result = sceHttpUriParse(writing ? &native : nullptr,text->c_str(),writing ? buffer.data() : nullptr,&need,capacity);
        if (!result && writing) {
            value.opaque = native.opaque; value.port = native.port;
            for (size_t i=0;i<members.size();++i) {
                const char* p = native.*members[i];
                if (!p) continue;
                const u64 offset = uintptr_t(p)-uintptr_t(buffer.data());
                if (offset >= capacity || a[2] > UINT64_MAX-offset) return bad;
                value.strings[i] = a[2]+offset;
            }
        }
    } else if (nid == "YuOW3dDAKYc") {
        auto text = HttpGuestText(space,a[3]);
        if (!text) return bad;
        result = sceHttpUriEscape(out ? buffer.data() : nullptr,&need,capacity,text->c_str());
    } else {
        if (!a[3] || !space.ReadData({a[3]},std::as_writable_bytes(std::span{&value,1})))
            return u32(ORBIS_HTTP_ERROR_INVALID_URL);
        if (value.opaque > 1) return bad;
        native.opaque = value.opaque; native.port = value.port;
        std::array<std::string,7> strings;
        for (size_t i=0;i<members.size();++i) if (value.strings[i]) {
            auto text = HttpGuestText(space,value.strings[i]); if (!text) return bad;
            strings[i] = std::move(*text); native.*members[i] = strings[i].data();
        }
        result = sceHttpUriBuild(out ? buffer.data() : nullptr,&need,capacity,&native,u32(a[4]));
    }
    std::vector<GuestAddressSpace::DataRequest> outputs;
    std::vector<std::span<const std::byte>> bytes;
    auto add = [&](u64 at, const void* p, size_t size) {
        outputs.push_back({{{at},size},GuestPermission::Write});
        bytes.emplace_back(static_cast<const std::byte*>(p),size);
    };
    if (required) add(required,&need,8);
    if (!result && writing) {
        if (need > capacity) return bad;
        if (parse) add(out,&value,sizeof(value));
        if (need) add(parse ? a[2] : out,buffer.data(),need);
    }
    for (size_t i=0;i<outputs.size();++i) {
        const auto& x = outputs[i].range;
        if (!GuestRange::Checked(x.base,x.size)) return bad;
        for (size_t j=0;j<i;++j) {
            const auto& y = outputs[j].range;
            if (x.base.value < y.End() && y.base.value < x.End()) return bad;
        }
    }
    if (!outputs.empty()) {
        // A URI element or pool can straddle distinct adjacent guest maps.
        // Pin all segments atomically, with identities from the split snapshot.
        std::vector<GuestAddressSpace::DataRequest> segments;
        std::deque<GuestAddressSpace::MappingIdentity> identities;
        std::vector<std::span<const std::byte>> chunks;
        for (size_t i=0;i<outputs.size();++i) {
            const auto range = outputs[i].range;
            for (u64 at=range.base.value; at<range.End();) {
                auto map = space.Query({at}); if (!map) return bad;
                const u64 end = std::min(range.End(),map.Value().range.End());
                identities.push_back({at,end,map.Value().mapping_generation});
                segments.push_back({{{at},end-at},GuestPermission::Write,
                                    std::span{&identities.back(),1}});
                chunks.push_back(bytes[i].subspan(at-range.base.value,end-at));
                at=end;
            }
        }
        auto pins = space.AcquireDataBatch(segments); if (!pins) return bad;
        for (size_t i=0;i<chunks.size();++i)
            std::memcpy(pins.Value()[i].WritableBytes().data(),chunks[i].data(),chunks[i].size());
    }
    return u32(result);
}
}
