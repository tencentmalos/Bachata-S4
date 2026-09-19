// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/network/http2_async_policy.h"

namespace Core::HostRuntime {
inline constexpr std::string_view Http2Nids[]{
    "3JCe3lCbQ8A", "YiBUtz-pGkc", "+wCt7fCijgk", "pDom5-078DA", "mmyOCxQMVYQ",
    "c8D9qIjo8EY", "IZ-qjhRqvjk", "nrPfOE8TQu0", "FSAFOzi0FpM", "9XYJwCf3lEA",
    "A+NVAFu4eCg", "bGN-6zbo7ms", "MOp-AUhdfi8", "rbqZig38AT8", "QygCNNmbGss",
    "o0DBQpFE13o", "-rdXUi2XW90", "-HIO4VT87v8", "izvHhqgDt44", "ACjtE27aErY",
    "b9AvoIaOuHI", "uRosf8GQbHQ", "VYMxTcBqSE0", "XPtW45xiLHk", "n8hMLe31OPA"};
inline bool IsHttp2Nid(std::string_view nid) {
    return std::ranges::find(Http2Nids, nid) != std::end(Http2Nids);
}
inline bool AdmitsHttp2(std::string_view nid, std::string_view suffix, bool offline) {
    return offline && IsHttp2Nid(nid) && suffix == "#libSceHttp2#1#libSceHttp2#Function";
}

// Session-owned offline control resources. No HTTP success/status/body is
// synthesized. Send fails before acceptance, so no completion worker, retained
// guest buffer, callback, VM pin or outstanding async ticket exists to drain.
// Pointer-bearing async completion ABI is deliberately not guessed: the shared
// desktop failure policy writes no event and promises no successful operation.
class GuestHttp2 {
    enum class Kind { Context, Template, Request };
    struct Resource {
        Kind kind;
        s32 parent{}, context{};
        u32 max_requests{};
        u64 content_length{};
        s32 failure{};
        std::array<u64, 8> options{};
        std::vector<std::pair<std::string, std::string>> headers;
    };
    std::mutex mutex;
    std::map<s32, Resource> resources;
    s32 next_id{1};
    static constexpr size_t MaxResources = 512;
    static constexpr size_t MaxHeaders = 32;

public:
    u64 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a) {
        using namespace GuestCpu;
        auto text = [&](u64 address, size_t limit) -> std::optional<std::string> {
            if (!address || address > UINT64_MAX - limit)
                return {};
            std::string result;
            for (size_t i = 0; i < limit; ++i) {
                char c{};
                if (!space.ReadData(GuestAddress{address + i},
                                    std::as_writable_bytes(std::span{&c, 1})))
                    return {};
                if (!c)
                    return result;
                result += c;
            }
            return {};
        };
        auto valid = [&](u64 address, u64 size, bool write) {
            return bool(
                space.ValidateRange({GuestAddress{address}, size},
                                    write ? GuestPermission::Write : GuestPermission::Read));
        };
        std::lock_guard lock(mutex); // this HTTP handle table only
        const s32 id = static_cast<s32>(a[0]);
        auto it = resources.find(id);
        auto create = [&](Resource r) -> u64 {
            if (resources.size() >= MaxResources || next_id == INT32_MAX)
                return u32(ORBIS_HTTP_ERROR_OUT_OF_MEMORY);
            const auto fresh = next_id++;
            resources.emplace(fresh, std::move(r));
            return fresh;
        };
        if (nid == "3JCe3lCbQ8A") { // Init: control pool only, no TLS/connection
            if (s32(a[0]) < 0 || s32(a[1]) < 0 || !a[2] || a[2] > (64ULL << 20) || s32(a[3]) <= 0 ||
                a[3] > 256)
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
            return create({.kind = Kind::Context, .max_requests = u32(a[3])});
        }
        if (!IsHttp2Nid(nid))
            return u32(ORBIS_HTTP_ERROR_UNKNOWN);
        if (it == resources.end())
            return u32(ORBIS_HTTP_ERROR_INVALID_ID);
        auto& r = it->second;
        if (nid == "+wCt7fCijgk") { // CreateTemplate
            if (r.kind != Kind::Context)
                return u32(ORBIS_HTTP_ERROR_INVALID_ID);
            if (a[2] < 1 || a[2] > 3)
                return u32(ORBIS_HTTP_ERROR_INVALID_VERSION);
            if (a[3] > 1 || !text(a[1], 1024))
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
            return create({.kind = Kind::Template, .parent = id, .context = id});
        }
        if (nid == "mmyOCxQMVYQ") { // CreateRequestWithURL
            if (r.kind != Kind::Template)
                return u32(ORBIS_HTTP_ERROR_INVALID_ID);
            const auto method = text(a[1], 16);
            const auto url = text(a[2], 8192);
            if (!method || !url)
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
            constexpr std::string_view methods[]{"GET",    "POST",  "HEAD",    "OPTIONS", "PUT",
                                                 "DELETE", "TRACE", "CONNECT", "PATCH"};
            if (std::ranges::find(methods, *method) == std::end(methods))
                return u32(ORBIS_HTTP_ERROR_UNKNOWN_METHOD);
            const auto scheme = url->starts_with("https://")  ? 8u
                                : url->starts_with("http://") ? 7u
                                                              : 0u;
            if (!scheme)
                return u32(ORBIS_HTTP_ERROR_UNKNOWN_SCHEME);
            if (url->size() <= scheme || (*url)[scheme] == '/' ||
                url->find_first_of("\r\n\t ") != std::string::npos)
                return u32(ORBIS_HTTP_ERROR_INVALID_URL);
            const auto count = std::ranges::count_if(resources, [&](const auto& entry) {
                return entry.second.kind == Kind::Request && entry.second.context == r.context;
            });
            if (count >= resources.at(r.context).max_requests)
                return u32(ORBIS_HTTP_ERROR_OUT_OF_SIZE);
            auto request = r;
            request.kind = Kind::Request;
            request.parent = id;
            request.content_length = a[3];
            return create(std::move(request));
        }
        if (nid == "YiBUtz-pGkc" || nid == "pDom5-078DA" || nid == "c8D9qIjo8EY") {
            const auto kind = nid == "YiBUtz-pGkc"   ? Kind::Context
                              : nid == "pDom5-078DA" ? Kind::Template
                                                     : Kind::Request;
            if (r.kind != kind)
                return u32(ORBIS_HTTP_ERROR_INVALID_ID);
            if (std::ranges::any_of(resources,
                                    [&](const auto& e) { return e.second.parent == id; }))
                return u32(ORBIS_HTTP_ERROR_BUSY);
            resources.erase(it);
            return 0;
        }
        if (nid == "nrPfOE8TQu0") { // AddRequestHeader; never log/store guest addresses
            if (r.kind == Kind::Context)
                return u32(ORBIS_HTTP_ERROR_INVALID_ID);
            const auto name = text(a[1], 256), value = text(a[2], 2048);
            if (!name || !value || name->empty() || a[3] > 1 ||
                name->find_first_of(":\r\n") != std::string::npos ||
                value->find_first_of("\r\n") != std::string::npos)
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
            if (r.failure)
                return u32(ORBIS_HTTP_ERROR_AFTER_SEND);
            auto equal_name = [&](const auto& h) {
                return h.first.size() == name->size() &&
                       std::equal(h.first.begin(), h.first.end(), name->begin(),
                                  [](unsigned char x, unsigned char y) {
                                      const auto lower = [](unsigned char c) {
                                          return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
                                      };
                                      return lower(x) == lower(y);
                                  });
            };
            const auto replace_count = a[3] == 0 ? std::ranges::count_if(r.headers, equal_name) : 0;
            if (r.headers.size() - replace_count >= MaxHeaders)
                return u32(ORBIS_HTTP_ERROR_OUT_OF_SIZE);
            if (a[3] == 0)
                std::erase_if(r.headers, equal_name);
            r.headers.emplace_back(*name, *value);
            return 0;
        }
        constexpr std::string_view setters[]{"-HIO4VT87v8", "izvHhqgDt44", "ACjtE27aErY",
                                             "b9AvoIaOuHI", "uRosf8GQbHQ", "VYMxTcBqSE0",
                                             "XPtW45xiLHk", "n8hMLe31OPA"};
        if (const auto option = std::ranges::find(setters, nid); option != std::end(setters)) {
            if (r.kind == Kind::Context)
                return u32(ORBIS_HTTP_ERROR_INVALID_ID);
            if (a[1] > UINT32_MAX || ((nid == "b9AvoIaOuHI" || nid == "uRosf8GQbHQ") && a[1] > 1))
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
            if (r.failure)
                return u32(ORBIS_HTTP_ERROR_AFTER_SEND);
            r.options[option - std::begin(setters)] = a[1];
            return 0; // retained configuration, never a transport capability
        }
        if (r.kind != Kind::Request)
            return u32(ORBIS_HTTP_ERROR_INVALID_ID);
        if (nid == "FSAFOzi0FpM") {
            if (r.failure)
                return u32(ORBIS_HTTP_ERROR_AFTER_SEND);
            r.content_length = a[1];
            return 0;
        }
        if (nid == "IZ-qjhRqvjk") {
            r.failure = ORBIS_HTTP_ERROR_ABORTED;
            return 0;
        }
        if (nid == "A+NVAFu4eCg" || nid == "rbqZig38AT8") {
            if (a[2] && !valid(a[1], a[2], false))
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
            if (!r.failure)
                r.failure = Libraries::Http2::UnavailableAsyncResult();
            return u32(r.failure); // rejected now; no queue and no promised completion
        }
        if (nid == "QygCNNmbGss" || nid == "bGN-6zbo7ms") {
            if (a[2] && !valid(a[1], a[2], true))
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
        } else if (nid == "9XYJwCf3lEA") {
            if (!valid(a[1], sizeof(s32), true))
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
        } else if (nid == "o0DBQpFE13o" || nid == "-rdXUi2XW90") {
            const auto first_size = nid == "o0DBQpFE13o" ? sizeof(s32) : sizeof(u64);
            if (!valid(a[1], first_size, true) || !valid(a[2], sizeof(u64), true))
                return u32(ORBIS_HTTP_ERROR_INVALID_VALUE);
        }
        // Wait's event ABI remains unimplemented; never touch unknown layouts.
        // No operation was accepted, so a negative result is always truthful.
        return u32(r.failure ? r.failure : ORBIS_HTTP_ERROR_BEFORE_SEND);
    }
};
} // namespace Core::HostRuntime
