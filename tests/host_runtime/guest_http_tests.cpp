// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <future>
#include <filesystem>
#include <unistd.h>
#include "common/path_util.h"
#include <thread>
#include "core/host_runtime/guest_http.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Http;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); } } while(0)
int main() {
#ifdef __ANDROID__
    Common::FS::InitializeAndroidUserPaths(std::filesystem::current_path() / "cache" / ("http-test-" + std::to_string(getpid())));
#endif
    AddressSpaceConfig config{}; config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config); if (!made) return 2;
    auto space = std::move(made).Value(); const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{base+0x4000}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    auto text = [&](u64 p, const char* s) { CHECK(space->WriteData(GuestAddress{p}, std::as_bytes(std::span{s, std::strlen(s)+1}))); };
    auto read = [&]<typename T>(u64 p) { T v{}; CHECK(space->ReadData(GuestAddress{p}, std::as_writable_bytes(std::span{&v,1}))); return v; };
    text(base, "GuestHttpTest"); text(base+128,"http://offline.invalid/path"); text(base+256,"X-Test"); text(base+320,"value");
    GuestHttp http, foreign;
    auto call = [&](std::string_view nid, std::array<u64,6> a = {}) { CHECK(IsHttpNid(nid)); return http.Dispatch(*space,nid,a); };
    for (auto nid : HttpNids) {
        CHECK(AdmitsHttp(nid,"#libSceHttp#1#libSceHttp#Function",true));
        CHECK(!AdmitsHttp(nid,"#libSceHttp#1#libSceHttp#Function",false));
        CHECK(!AdmitsHttp(nid,"#libkernel#1#libkernel#Function",true));
    }
    CHECK(call("A9cVMUtEp4Y", {1,1,0}) == u32(Libraries::Http::sceHttpInit(1,1,0)));
    const auto ctx = call("A9cVMUtEp4Y", {1,1,4096}); CHECK(s32(ctx)>0);
    const auto ctx2 = call("A9cVMUtEp4Y", {1,1,4096}); CHECK(s32(ctx2)>0 && ctx2 != ctx);
    CHECK(foreign.Dispatch(*space,"Ik-KpLTlf7Q",{ctx}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    CHECK(call("0gYjPTR-6cY",{ctx,1,2,0}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    const auto tmpl = call("0gYjPTR-6cY",{ctx,base,2,0}); CHECK(s32(tmpl)>0);
    const auto conn = call("qgxDBjorUxs",{tmpl,base+128,1}); CHECK(s32(conn)>0);
    const auto req = call("Aeu5wVKkF9w",{conn,0,base+128,0}); CHECK(s32(req)>0);
    CHECK(call("s2-NPIvz+iA",{req,1}) == 0);
    CHECK(call("Wq4RNB3snSQ",{req,base+512}) == 0 && read.operator()<int>(base+512)==1);
    CHECK(call("f42K37mm5RM",{req,0x80}) == 0);
    CHECK(call("mSQCxzWTwVI",{req,0x80}) == 0);
    CHECK(call("EY28T2bkN7k",{req,base+256,base+320,0}) == 0);
    CHECK(call("PTiFIUxCpJc",{req,0}) == 0);
    CHECK(call("6381dWF+xsQ",{ctx,1}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("6381dWF+xsQ",{ctx,base+512}) == 0);
    const auto ep = read.operator()<u64>(base+512); CHECK(ep != 0);
    CHECK(call("wYhXVfS2Et4",{ctx2,ep}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    CHECK(call("-xm7kZQNpHI",{req,ep,0xdeadbeef12345678ULL}) == 0);
    CHECK(call("qISjDHrxONc",{ep,base+1024,1,0}) == 0);
    CHECK(call("1e2BNwI-XzE",{req,1,1}) == u32(ORBIS_HTTP_ERROR_INVALID_VALUE));
    CHECK(call("1e2BNwI-XzE",{req,0,0}) == 0);
    CHECK(call("qISjDHrxONc",{ep,base+1024,1,1000000}) == 1);
    struct Event { u32 bits,detail; s32 id; u32 pad; u64 argument; };
    const auto event = read.operator()<Event>(base+1024);
    CHECK(event.bits == (ORBIS_HTTP_NB_EVENT_RESOLVER_ERR | ORBIS_HTTP_NB_EVENT_HUP));
    CHECK(event.detail == event.bits && event.id == s32(req) && event.pad == 0 && event.argument == 0xdeadbeef12345678ULL);
    CHECK(call("0onIrKx9NIE",{req,base+512}) == 0 && read.operator()<u32>(base+512)==u32(ORBIS_HTTP_ERROR_RESOLVER_ENODNS));
    CHECK(call("0a2TBNfE3BU",{req,base+512}) == u32(ORBIS_HTTP_ERROR_BEFORE_SEND));
    CHECK(call("P5pdoykPYTk",{req,base+1024,128}) == 0); // desktop empty failed response
    // An infinite wait owns no VM pin and cancellation releases the guest owner.
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] { return http.Dispatch(*space,"qISjDHrxONc",{ep,base+0x4000,1,u32(-1)},stop.get_token()); });
    CHECK(waiting.wait_for(30ms) == std::future_status::timeout);
    auto unmap = std::async(std::launch::async,[&] { return space->Unmap({GuestAddress{base+0x4000},0x4000}); });
    CHECK(unmap.wait_for(200ms)==std::future_status::ready);
    stop.request_stop(); CHECK(unmap.get());
    CHECK(waiting.wait_for(1s)==std::future_status::ready);
    CHECK(waiting.get()==u32(ORBIS_HTTP_ERROR_ABORTED));
    CHECK(call("Ik-KpLTlf7Q",{ctx}) == 0);
    CHECK(call("Wq4RNB3snSQ",{req,base+512}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    // Other context remains usable after explicit child/context retirement.
    CHECK(s32(call("0gYjPTR-6cY",{ctx2,base,2,0}))>0);
    CHECK(call("Ik-KpLTlf7Q",{ctx2}) == 0);
    CHECK(call("Ik-KpLTlf7Q",{ctx2}) == u32(ORBIS_HTTP_ERROR_INVALID_ID));
    std::printf("guest_http_tests: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
