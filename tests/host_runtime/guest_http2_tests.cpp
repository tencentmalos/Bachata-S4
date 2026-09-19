// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <thread>
#include "core/host_runtime/guest_http2.h"
#include "core/host_runtime/guest_http2_compat.h"
#include "core/libraries/network/http2.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{base + 0x4000}, 0x4000}, GuestPermission::Read));
    auto put = [&](u64 offset, std::string_view text) {
        CHECK(space->Write(GuestAddress{base + offset},
                           std::as_bytes(std::span{text.data(), text.size() + 1})));
    };
    put(0, "agent");
    put(32, "POST");
    put(64, "https://example.invalid/auth");
    put(256, "Content-Type");
    put(512, "application/json");
    put(1024, "Header\r\nInjected");
    GuestHttp2 http;
    auto call = [&](std::string_view nid, std::array<u64, 6> a = {}) {
        CHECK(IsHttp2Nid(nid));
        return http.Dispatch(*space, nid, a);
    };
    CHECK(call("3JCe3lCbQ8A", {1, 1, 0, 2}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("3JCe3lCbQ8A", {1, 1, 4096, 0}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("3JCe3lCbQ8A", {1, 1, 4096, 257}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    const auto ctx = call("3JCe3lCbQ8A", {1, 1, 4096, 2});
    CHECK(s32(ctx) > 0);
    CHECK(call("+wCt7fCijgk", {ctx, 1, 3, 0}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("+wCt7fCijgk", {ctx, base, 4, 0}) == u32(ORBIS_HTTP_ERROR_INVALID_VERSION));
    const auto tmpl = call("+wCt7fCijgk", {ctx, base, 3, 0});
    CHECK(tmpl != ctx && s32(tmpl) > 0);
    CHECK(call("YiBUtz-pGkc", {ctx}) == u32(ORBIS_HTTP_ERROR_BUSY));
    CHECK(call("nrPfOE8TQu0", {tmpl, base + 1024, base + 512, 0}) ==
          u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("nrPfOE8TQu0", {tmpl, base + 256, base + 512, 0}) == 0);
    for (auto nid : {"-HIO4VT87v8", "izvHhqgDt44", "ACjtE27aErY", "b9AvoIaOuHI", "uRosf8GQbHQ",
                     "VYMxTcBqSE0", "XPtW45xiLHk", "n8hMLe31OPA"}) {
        CHECK(call(nid, {tmpl, 1}) == 0);
        CHECK(call(nid, {ctx, 1}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    }
    CHECK(call("b9AvoIaOuHI", {tmpl, 2}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("mmyOCxQMVYQ", {ctx, base + 32, base + 64, 0}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    CHECK(call("mmyOCxQMVYQ", {tmpl, 1, base + 64, 0}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    put(128, "ftp://example.invalid");
    CHECK(call("mmyOCxQMVYQ", {tmpl, base + 32, base + 128, 0}) ==
          u32(ORBIS_HTTP_ERROR_UNKNOWN_SCHEME));
    const auto req = call("mmyOCxQMVYQ", {tmpl, base + 32, base + 64, 2});
    const auto other = call("mmyOCxQMVYQ", {tmpl, base + 32, base + 64, 2});
    CHECK(req != other && req != tmpl && s32(req) > 0);
    CHECK(call("mmyOCxQMVYQ", {tmpl, base + 32, base + 64, 0}) ==
          u32(ORBIS_HTTP_ERROR_OUT_OF_SIZE));
    CHECK(call("pDom5-078DA", {tmpl}) == u32(ORBIS_HTTP_ERROR_BUSY));
    CHECK(call("FSAFOzi0FpM", {req, 512}) == 0);
    CHECK(call("MOp-AUhdfi8", {req, UINT64_MAX, 1, 0}) == u32(ORBIS_HTTP_ERROR_BEFORE_SEND));
    CHECK(call("A+NVAFu4eCg", {req, UINT64_MAX - 1, 8}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("MOp-AUhdfi8", {req}) == u32(ORBIS_HTTP_ERROR_BEFORE_SEND));
    CHECK(call("A+NVAFu4eCg", {req, base + 512, 2}) ==
          u32(Libraries::Http2::UnavailableAsyncResult()));
    std::array<u8, 64> sentinel{};
    sentinel.fill(0xa5);
    CHECK(space->Write(GuestAddress{base + 2048}, std::as_bytes(std::span{sentinel})));
    CHECK(call("MOp-AUhdfi8", {req, base + 2048, base + 2080, UINT32_MAX}) ==
          u32(ORBIS_HTTP_ERROR_NETWORK));
    CHECK(call("9XYJwCf3lEA", {req, base + 2048}) == u32(ORBIS_HTTP_ERROR_NETWORK));
    CHECK(call("o0DBQpFE13o", {req, base + 2048, base + 2080}) == u32(ORBIS_HTTP_ERROR_NETWORK));
    CHECK(call("-rdXUi2XW90", {req, base + 2048, base + 2080}) == u32(ORBIS_HTTP_ERROR_NETWORK));
    CHECK(call("bGN-6zbo7ms", {req, base + 2048, 32, 0, 0}) == u32(ORBIS_HTTP_ERROR_NETWORK));
    std::array<u8, 64> after{};
    CHECK(space->Read(GuestAddress{base + 2048}, std::as_writable_bytes(std::span{after})));
    CHECK(after == sentinel); // failed operations cannot fabricate a status/event/body
    CHECK(call("9XYJwCf3lEA", {req, base + 0x4000}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("bGN-6zbo7ms", {req, base + 0x4000, 16}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("FSAFOzi0FpM", {req, 1}) == u32(ORBIS_HTTP_ERROR_AFTER_SEND));
    CHECK(call("IZ-qjhRqvjk", {req}) == 0);
    CHECK(call("MOp-AUhdfi8", {req}) == u32(ORBIS_HTTP_ERROR_ABORTED));
    CHECK(call("c8D9qIjo8EY", {req}) == 0);
    CHECK(call("MOp-AUhdfi8", {req}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    CHECK(call("rbqZig38AT8", {other, 0, 0}) == u32(ORBIS_HTTP_ERROR_NETWORK));
    CHECK(call("QygCNNmbGss", {other, base + 2048, 16}) == u32(ORBIS_HTTP_ERROR_NETWORK));
    CHECK(call("c8D9qIjo8EY", {other}) == 0);
    CHECK(call("pDom5-078DA", {tmpl}) == 0);
    CHECK(call("YiBUtz-pGkc", {ctx}) == 0);
    CHECK(call("YiBUtz-pGkc", {ctx}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    for (auto nid : Http2Nids) {
        CHECK(AdmitsHttp2(nid, "#libSceHttp2#1#libSceHttp2#Function", true));
        CHECK(!AdmitsHttp2(nid, "#libkernel#1#libkernel#Function", true));
        CHECK(!AdmitsHttp2(nid, "#libSceHttp2#1#libSceHttp2#Function", false));
    }
    CHECK(Libraries::Http2::sceHttp2SendRequestAsync() == ORBIS_HTTP_ERROR_NETWORK);
    CHECK(Libraries::Http2::sceHttp2ReadDataAsync() == ORBIS_HTTP_ERROR_NETWORK);
    CHECK(Libraries::Http2::sceHttp2WaitAsync() == ORBIS_HTTP_ERROR_NETWORK);
    CHECK(!AdmitsHttp2("unknown", "#libSceHttp2#1#libSceHttp2#Function", true));
    // No background queue survives object destruction or crosses Session domains.
    for (int round = 0; round < 3; ++round) {
        GuestHttp2 fresh;
        CHECK(fresh.Dispatch(*space, "MOp-AUhdfi8", {req}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    }
    std::array<u8, Http2CompatSize> region{};
    region.fill(0x90);
    constexpr std::array<u8, 6> stub{0x31, 0xc0, 0xc3, 0xaf, 0x77, 0x00};
    for (const auto& e : Http2CompatBindings)
        std::memcpy(region.data() + e.slot - Http2CompatStart, stub.data(), stub.size());
    const auto original = region;
    CHECK(!RestoreHttp2ImportBytes("different executable", region, Http2CompatBindings));
    CHECK(region == original);
    auto bindings = std::to_array(Http2CompatBindings);
    bindings.back().nid = "wrong-library-or-symbol";
    bool rejected{};
    try {
        RestoreHttp2ImportBytes(Http2CompatTextSha, region, bindings);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected && region == original); // all-or-none, last import mismatch
    bindings = std::to_array(Http2CompatBindings);
    bindings.back() = bindings.front();
    rejected = false;
    try {
        RestoreHttp2ImportBytes(Http2CompatTextSha, region, bindings);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected && region == original);
    region.back() ^= 1;
    const auto changed = region;
    rejected = false;
    try {
        RestoreHttp2ImportBytes(Http2CompatTextSha, region, Http2CompatBindings);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected && region == changed);
    region = original;
    CHECK(RestoreHttp2ImportBytes(Http2CompatTextSha, region, Http2CompatBindings));
    for (const auto& e : Http2CompatBindings) {
        const auto* bytes = region.data() + e.slot - Http2CompatStart;
        s32 displacement{};
        std::memcpy(&displacement, bytes + 2, sizeof(displacement));
        CHECK(bytes[0] == 0xff && bytes[1] == 0x25 && s64(e.slot + 6) + displacement == s64(e.got));
    }
    // Preparation owns a VM token: writable pin must fail, token publication
    // must succeed, and a readback must see the same restored GOT jumps.
    auto drain = space->BeginDrain();
    CHECK(drain);
    if (drain) {
        auto token = space->FinishDrain(drain.Value(), 1'000'000'000, 0);
        CHECK(token);
        if (token) {
            CHECK(!space->AcquirePinnedSpan({GuestAddress{base}, region.size()}, true));
            CHECK(space->PublishCode(token.Value(), {GuestAddress{base}, region.size()},
                                     std::as_bytes(std::span{region})));
            std::array<u8, Http2CompatSize> published{};
            CHECK(space->Read(GuestAddress{base}, std::as_writable_bytes(std::span{published})));
            CHECK(published == region);
        }
    }
    // Full-image hash is checked by the runtime before passing this pure planner.
    std::printf("guest_http2_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
