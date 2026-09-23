// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <filesystem>
#include <unistd.h>
#include "common/path_util.h"
#include "common/elf_info.h"
#include "core/host_runtime/guest_user_callbacks.h"
#include "core/host_runtime/guest_ime_keyboard.h"
#include "core/libraries/ime/ime.h"
#include "core/host_runtime/guest_np_utility.h"
#include "core/host_runtime/guest_np_webapi.h"
#include "core/libraries/np/np_web_api/np_web_api_internal.h"
#include "core/host_runtime/guest_system_service.h"
#include "core/host_runtime/guest_hmd_geometry.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/libraries/move/move.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::UserService;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#__VA_ARGS__); } } while(0)
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Common::ElfInfo metadata;
    metadata.InitializeGuestMetadata({},Common::ElfInfo::FW_900);
    Common::Singleton<Common::ElfInfo>::Binding metadata_binding(metadata);
#ifdef __ANDROID__
    Common::FS::InitializeAndroidUserPaths(std::filesystem::current_path()/"cache"/("psvr-test-"+std::to_string(getpid())));
#endif
    AddressSpaceConfig config{}; config.reservation_size=16<<20;
    auto made=GuestAddressSpace::Create(config); if (!made) return 2;
    auto space=std::move(made).Value(); const u64 base=space->ReservationBase().value;
    CHECK(space->Map({{base},0x4000},GuestPermission::Read|GuestPermission::Write));
    CHECK(space->Map({{base+0x4000},0x4000},GuestPermission::Read|GuestPermission::Execute));
    {
        using namespace Libraries::Hmd;
        const u64 left_address = base + 0x101, right_address = base + 0x201;
        CHECK(GuestHmdEyeOffsets(*space, 0, left_address, right_address) == u32(ORBIS_HMD_ERROR_NOT_INITIALIZED));
        GuestVrSensor::Instance().SetSbsEnabled(true);
        OrbisHmdInitializeParam init{};
        CHECK(sceHmdInitialize315(&init) == 0);
        const s32 handle = sceHmdOpen(1000, 0, 0, nullptr);
        CHECK(handle > 0);
        CHECK(GuestHmdEyeOffsets(*space, handle + 1, left_address, right_address) == u32(ORBIS_HMD_ERROR_HANDLE_INVALID));
        std::array<u8, 32> sentinel{}; sentinel.fill(0xa5);
        CHECK(space->WriteData({left_address}, std::as_bytes(std::span{sentinel})));
        CHECK(space->WriteData({right_address}, std::as_bytes(std::span{sentinel})));
        for (u64 bad : {u64(0), base + 0x3ff8, base + 0x4000, ~u64(0) - 7}) {
            CHECK(GuestHmdEyeOffsets(*space, handle, left_address, bad) == u32(ORBIS_HMD_ERROR_PARAMETER_NULL));
            std::array<u8, 32> after{};
            CHECK(space->ReadData({left_address}, std::as_writable_bytes(std::span{after})) && after == sentinel);
        }
        CHECK(GuestHmdEyeOffsets(*space, handle, left_address, right_address) == 0);
        OrbisHmdEyeOffset left{}, right{};
        CHECK(space->ReadData({left_address}, std::as_writable_bytes(std::span{&left, 1})));
        CHECK(space->ReadData({right_address}, std::as_writable_bytes(std::span{&right, 1})));
        CHECK(left.offset_x == -0.0315f && right.offset_x == 0.0315f);
        CHECK(left.offset_y == 0 && left.offset_z == 0 && right.offset_y == 0 && right.offset_z == 0);
        CHECK(std::all_of(std::begin(left.reserve), std::end(left.reserve), [](u8 b) { return b == 0xa5; }));
        CHECK(std::all_of(std::begin(right.reserve), std::end(right.reserve), [](u8 b) { return b == 0xa5; }));
        CHECK(GuestHmdEyeOffsets(*space, handle, left_address, base + 0x3ff4) == 0);
        CHECK(sceHmdClose(handle) == 0);
        CHECK(GuestHmdEyeOffsets(*space, handle, left_address, right_address) == u32(ORBIS_HMD_ERROR_HANDLE_INVALID));
        CHECK(sceHmdTerminate() == 0);
        CHECK(GuestHmdEyeOffsets(*space, handle, left_address, right_address) == u32(ORBIS_HMD_ERROR_NOT_INITIALIZED));
        using namespace Libraries::Move;
        CHECK(sceMoveInit() == 0);
        const s32 move_handle = sceMoveOpen(1000, 0, 0);
        CHECK(move_handle > 0);
        // Publish the same units supplied by OrbisPadAdapter/JNI. Both public
        // Move readers must preserve them, including sub-millisecond changes.
        for (u64 timestamp_us : {2'000'001ULL, 2'000'002ULL, 745'000'000'123ULL}) {
            GuestVrSensor::Instance().UpdateMoveInput(0, 0, 0, 0, 0, 0, 0, timestamp_us);
            OrbisMoveData data{};
            CHECK(sceMoveReadStateLatest(move_handle, &data) == 0);
            CHECK(data.timestamp == s64(timestamp_us));
            s32 count{};
            CHECK(sceMoveReadStateRecent(move_handle, 0, &data, &count) == 0);
            CHECK(count == 1 && data.timestamp == s64(timestamp_us));
        }
        CHECK(sceMoveTerm() == 0);
        GuestVrSensor::Instance().SetSbsEnabled(false);
    }
    GuestNpUtility utility(0x09000000);
    auto call=[&](std::string_view nid,std::array<u64,6> a={}) { return utility.Dispatch(*space,nid,a); };
    CHECK(call("6p9jvljuvsw",{1000})==u32(ORBIS_NP_ERROR_SIGNED_OUT));
    CHECK(call("6p9jvljuvsw",{u32(-1)})==u32(ORBIS_NP_ERROR_INVALID_ARGUMENT));
    CHECK(call("iCq5xW5KQW4",{1})==u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ID));
    CHECK(call("M5Jyo9TKYPI")==0);
    CHECK(call("6p9jvljuvsw",{1000})==u32(ORBIS_NP_COMMUNITY_ERROR_NOT_INITIALIZED));
    CHECK(call("W6iWw8aUQtA")==0);
    CHECK(call("Jj4mkpFO2gE",{1,0,0})==u32(ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT));
    const char comment[]="local";
    CHECK(space->WriteData({base+3000},std::as_bytes(std::span{comment})));
    CHECK(call("1dMndqL-QgE",{1,base+3000,0})==u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ID));
    CHECK(call("1dMndqL-QgE",{1,base+3000,1})==u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT));
    std::array<u64,7> bandwidth{}; bandwidth[0]=56;
    CHECK(space->WriteData({base},std::as_bytes(std::span{bandwidth})));
    CHECK(call("jktww3yJXnc",{base})==u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_CONTEXT_NOT_AVAILABLE));
    CHECK(call("jktww3yJXnc",{base+0x7ff0})==u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT));
    CHECK(call("BYIZGKm6bO4",{base})==u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_NOT_INITIALIZED));
    CHECK(call("kvdMF48mB3Y")==u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_NOT_INITIALIZED));
    std::array<u64,7> unchanged{};
    CHECK(space->ReadData({base},std::as_writable_bytes(std::span{unchanged})) && unchanged==bandwidth);
    {
        GuestHttp http;
        GuestNpWebApiControl web(http), foreign(http);
        CHECK(IsNpWebApiControlNid("gVNNyxf-1Sg"));
        web.CheckTimeout();
        auto webcall=[&](std::string_view nid,std::array<u64,6> a={}) { CHECK(IsNpWebApiControlNid(nid)); return web.Dispatch(*space,nid,a); };
        const auto http_id=http.Dispatch(*space,"A9cVMUtEp4Y",{1,1,1<<20});
        CHECK(s32(http_id)>0);
        CHECK(webcall("G3AnLNdRBjE",{1,1<<20})==u32(ORBIS_NP_WEBAPI_ERROR_INVALID_ARGUMENT));
        const auto ctx=webcall("G3AnLNdRBjE",{http_id,1<<20}); CHECK(s32(ctx)>0);
        CHECK(foreign.Dispatch(*space,"asz3TtIqGF8",{ctx})==u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND));
        const char group[]="commerce";
        const char path[]="/v1/test/entitlements";
        CHECK(space->WriteData({base+3000},std::as_bytes(std::span{group})));
        CHECK(space->WriteData({base+3100},std::as_bytes(std::span{path})));
        const u64 untouched=0xa5a5a5a5a5a5a5a5ULL;
        CHECK(space->WriteData({base+3500},std::as_bytes(std::span{&untouched,1})));
        CHECK(webcall("rdgs5Z1MyFw",{u32(-1),base+3000,base+3100,0,0,base+3500})==u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND));
        u64 rid{};
        CHECK(space->ReadData({base+3500},std::as_writable_bytes(std::span{&rid,1})) && rid==untouched);
        const auto user=webcall("zk6c65xoyO0",{ctx,1000}); CHECK(s32(user)>0);
        CHECK(webcall("zk6c65xoyO0",{ctx,1000})==u32(ORBIS_NP_WEBAPI_ERROR_USER_CONTEXT_ALREADY_EXIST));
        CHECK(foreign.Dispatch(*space,"XUjdsSTTZ3U",{user})==u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND));
        CHECK(webcall("rdgs5Z1MyFw",{user,base+3000,base+3100,0,0,base+0x7ffc})==u32(ORBIS_NP_WEBAPI_ERROR_INVALID_ARGUMENT));
        CHECK(webcall("rdgs5Z1MyFw",{user,base+3000,base+3100,0,0,base+3500})==0);
        CHECK(space->ReadData({base+3500},std::as_writable_bytes(std::span{&rid,1})) && rid!=untouched && rid>0);
        CHECK(foreign.Dispatch(*space,"noQgleu+KLE",{rid})==u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND));
        CHECK(webcall("qWcbJkBj1Lg",{rid,20'000'000})==0);
        CHECK(webcall("KjNeZ-29ysQ",{rid,base+0x8000,4})==u32(ORBIS_NP_WEBAPI_ERROR_INVALID_ARGUMENT));
        CHECK(webcall("KjNeZ-29ysQ",{rid,0,0})==u32(ORBIS_NP_WEBAPI_ERROR_NOT_SIGNED_IN));
        CHECK(space->WriteData({base+3600},std::as_bytes(std::span{&untouched,1})));
        CHECK(webcall("CQtPRSF6Ds8",{rid,base+3600,8})==u32(ORBIS_NP_WEBAPI_ERROR_NOT_SIGNED_IN));
        CHECK(webcall("743ZzEBzlV8",{rid,base+3000,base+3600})==u32(ORBIS_NP_WEBAPI_ERROR_NOT_SIGNED_IN));
        CHECK(webcall("VwJ5L0Higg0",{rid,base+3000,base+3600,8})==u32(ORBIS_NP_WEBAPI_ERROR_NOT_SIGNED_IN));
        u64 after{};
        CHECK(space->ReadData({base+3600},std::as_writable_bytes(std::span{&after,1})) && after==untouched);
        CHECK(webcall("noQgleu+KLE",{rid})==0);
        CHECK(webcall("noQgleu+KLE",{rid})==u32(ORBIS_NP_WEBAPI_ERROR_REQUEST_NOT_FOUND));
        // The shared native missing-request path must release its recursive
        // context lock so a different thread can keep using that context.
        CHECK(u32(Libraries::Np::NpWebApi::sceNpWebApiDeleteRequest(rid))==u32(ORBIS_NP_WEBAPI_ERROR_REQUEST_NOT_FOUND));
        std::thread other([&] { web.CheckTimeout(); }); other.join();
        CHECK(webcall("rdgs5Z1MyFw",{user,base+3000,base+3100,0,0,base+3500})==0);
        CHECK(space->ReadData({base+3500},std::as_writable_bytes(std::span{&rid,1})));
        CHECK(webcall("XUjdsSTTZ3U",{user})==0); // retires its remaining request
        CHECK(webcall("KjNeZ-29ysQ",{rid})==u32(ORBIS_NP_WEBAPI_ERROR_USER_CONTEXT_NOT_FOUND));
        CHECK(webcall("XUjdsSTTZ3U",{user})==u32(ORBIS_NP_WEBAPI_ERROR_USER_CONTEXT_NOT_FOUND));
        // Termination and destructor retire live offline users/requests too.
        for (int i=0;i<8;++i) {
            GuestNpWebApiControl scoped(http);
            const auto c=scoped.Dispatch(*space,"G3AnLNdRBjE",{http_id,4096}); CHECK(s32(c)>0);
            const auto u=scoped.Dispatch(*space,"zk6c65xoyO0",{c,1000}); CHECK(s32(u)>0);
            CHECK(scoped.Dispatch(*space,"rdgs5Z1MyFw",{u,base+3000,base+3100,0,0,base+3500})==0);
            if (i&1) CHECK(scoped.Dispatch(*space,"asz3TtIqGF8",{c})==0);
        }
        const auto handle=webcall("79M-JqvvGo0",{ctx}); CHECK(s32(handle)>0);
        struct Filter { char type[65]; u8 padding[7]; u64 keys,count; } filter{};
        std::strcpy(filter.type,"np:test:event"); filter.keys=base+1024; filter.count=1;
        CHECK(space->WriteData({base+512},std::as_bytes(std::span{&filter,1})));
        Libraries::Np::NpWebApi::OrbisNpWebApiExtdPushEventExtdDataKey key{}; std::strcpy(key.val,"key");
        CHECK(space->WriteData({base+1024},std::as_bytes(std::span{&key,1})));
        const auto id=webcall("M2BUB+DNEGE",{ctx,handle,0,u32(-1),base+512,1}); CHECK(s32(id)>0);
        std::thread pump([&] { for (int i = 0; i < 100; ++i) web.CheckTimeout(); });
        for (int i = 0; i < 100; ++i) web.CheckTimeout();
        pump.join();
        const char service[]="np:test";
        CHECK(space->WriteData({base+2048},std::as_bytes(std::span{service})));
        CHECK(webcall("M2BUB+DNEGE",{ctx,handle,base+2048,0,base+512,1})==u32(ORBIS_NP_WEBAPI_ERROR_SIGNED_IN_USER_NOT_FOUND));
        CHECK(webcall("pfaJtb7SQ80",{ctx,id})==0);
        CHECK(webcall("pfaJtb7SQ80",{ctx,id})==u32(ORBIS_NP_WEBAPI_ERROR_EXTD_PUSH_EVENT_FILTER_NOT_FOUND));
        CHECK(webcall("5Mn7TYwpl30",{ctx,handle})==0);
        CHECK(webcall("5Mn7TYwpl30",{ctx,handle})==u32(ORBIS_NP_WEBAPI_ERROR_HANDLE_NOT_FOUND));
        CHECK(webcall("asz3TtIqGF8",{ctx})==0);
        web.CheckTimeout();
        CHECK(webcall("79M-JqvvGo0",{ctx})==u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND));
        // Repeated scopes retire live local handles/filters without explicit child deletes.
        for (int i=0;i<32;++i) {
            GuestNpWebApiControl scoped(http);
            const auto c=scoped.Dispatch(*space,"G3AnLNdRBjE",{http_id,4096}); CHECK(s32(c)>0);
            const auto h=scoped.Dispatch(*space,"79M-JqvvGo0",{c}); CHECK(s32(h)>0);
            CHECK(s32(scoped.Dispatch(*space,"M2BUB+DNEGE",{c,h,0,u32(-1),base+512,1}))>0);
        }
    }
    std::mutex mutex; std::condition_variable changed;
    unsigned started{},ended{},delivered{},errors{};
    bool hold=true,release{};
    GuestCallbackOwner owner{
        .begin=[&](u64) { std::lock_guard lock(mutex); ++started; return base+256; },
        .call=[&](u64 function,std::span<const u64> args) -> u64 {
            OrbisUserServiceEvent event{};
            const bool read=bool(space->ReadData({args[0]},std::as_writable_bytes(std::span{&event,1})));
            std::unique_lock lock(mutex);
            if (!read || args[1]!=123 || function!=base+0x4000 || event.userId!=1000 || event.event!=OrbisUserServiceEventType::Login) ++errors;
            ++delivered; changed.notify_all();
            if (hold) changed.wait(lock,[&] { return release; });
            return 0;
        },
        .cancel=[&] { std::lock_guard lock(mutex); release=true; changed.notify_all(); },
        .end=[&] { std::lock_guard lock(mutex); ++ended; },
        .invalidate=[](u64,size_t) {},
        .fail=[&](std::string_view) { std::lock_guard lock(mutex); ++errors; }
    };
    {
        GuestUserCallbacks callbacks(*space,std::move(owner));
        CHECK(callbacks.Dispatch(false,base+0x4000,123)==u32(ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED));
        OrbisUserServiceLoginUserIdList users{{1000,-1,-1,-1}}; callbacks.Initialize(users);
        CHECK(callbacks.Dispatch(false,base,123)==u32(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT));
        CHECK(callbacks.Dispatch(false,base+0x4000,123)==0);
        { std::unique_lock lock(mutex); CHECK(changed.wait_for(lock,2s,[&] { return delivered==1; })); }
        auto remove=std::async(std::launch::async,[&] { return callbacks.Dispatch(true,base+0x4000,123); });
        CHECK(remove.wait_for(30ms)==std::future_status::timeout);
        { std::lock_guard lock(mutex); release=true; changed.notify_all(); }
        CHECK(remove.wait_for(2s)==std::future_status::ready); CHECK(remove.get()==0);
        CHECK(callbacks.Dispatch(true,base+0x4000,123)==u32(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT));
        callbacks.Terminate();
        CHECK(callbacks.Dispatch(false,base+0x4000,123)==u32(ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED));
        callbacks.RequestStop();
    }
    CHECK(started==1 && ended==1 && delivered==1 && errors==0);
    {
        std::promise<void> entered, cancel;
        auto cancelled=cancel.get_future().share();
        std::atomic_bool notified{};
        std::atomic_uint faulted{},finished{};
        GuestCallbackOwner cancellation_owner{
            .begin=[&](u64) { return base+256; },
            .call=[&](u64,std::span<const u64>) -> u64 {
                entered.set_value(); cancelled.wait();
                throw std::runtime_error("guest callback cancelled");
            },
            .cancel=[&] { if (!notified.exchange(true)) cancel.set_value(); },
            .end=[&] { ++finished; },
            .invalidate=[](u64,size_t) {},
            .fail=[&](std::string_view) { ++faulted; }
        };
        {
            GuestUserCallbacks callbacks(*space,std::move(cancellation_owner));
            callbacks.Initialize({{1000,-1,-1,-1}});
            CHECK(callbacks.Dispatch(false,base+0x4000,123)==0);
            CHECK(entered.get_future().wait_for(2s)==std::future_status::ready);
            callbacks.RequestStop();
        }
        CHECK(finished==1 && faulted==0);
    }
    {
        using namespace Libraries::SystemService;
        using Info = OrbisSystemServiceDisplaySafeAreaInfo;
        // Both entry points share full-display policy; reserved bytes must not
        // expose stale stack/caller data and adjacent bytes must be preserved.
        Info native;
        std::memset(&native, 0xa5, sizeof(native));
        CHECK(sceSystemServiceGetDisplaySafeAreaInfo(nullptr) ==
              ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER);
        CHECK(sceSystemServiceGetDisplaySafeAreaInfo(&native) == 0);
        CHECK(native.ratio == 1.0f &&
              std::all_of(std::begin(native.reserved), std::end(native.reserved),
                          [](u8 byte) { return byte == 0; }));
        const u64 safe = base + 0x800000;
        CHECK(space->Map({{safe}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
        CHECK(space->Map({{safe + 0x4000}, 0x4000}, GuestPermission::Read));
        std::array<std::byte, sizeof(Info) + 2> bytes;
        bytes.fill(std::byte{0xa5});
        CHECK(space->WriteData({safe}, bytes));
        CHECK(GuestDisplaySafeAreaInfo(*space, safe + 1) == 0);
        CHECK(space->ReadData({safe}, bytes));
        CHECK(bytes.front() == std::byte{0xa5} && bytes.back() == std::byte{0xa5});
        CHECK(std::memcmp(bytes.data() + 1, &native, sizeof(native)) == 0);
        for (u64 invalid : {u64{0}, safe + 0x4000, safe + 0x8000,
                            safe + 0x4000 - sizeof(Info) + 1, ~u64{0} - 64}) {
            CHECK(GuestDisplaySafeAreaInfo(*space, invalid) ==
                  u32(ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER));
        }
        // Partial writable prefix must not be changed on a rejected full record.
        const u64 tail = safe + 0x4000 - sizeof(Info);
        bytes.fill(std::byte{0x5a});
        CHECK(space->WriteData({tail}, std::span{bytes}.first(sizeof(Info))));
        CHECK(GuestDisplaySafeAreaInfo(*space, tail + 1) ==
              u32(ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER));
        CHECK(space->ReadData({tail}, std::span{bytes}.first(sizeof(Info))));
        CHECK(std::all_of(bytes.begin(), bytes.end(),
                          [](std::byte byte) { return byte == std::byte{0x5a}; }));
        CHECK(GuestDisplaySafeAreaInfo(*space, tail) == 0);
        CHECK(space->Unmap({{safe}, 0x4000}));
        CHECK(GuestDisplaySafeAreaInfo(*space, safe) ==
              u32(ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER));
    }
    {
        using namespace Libraries::Ime;
        GuestImeKeyboard keyboard, other;
        auto call = [&](std::string_view nid, std::array<u64, 6> args = {}) {
            return keyboard.Invoke(*space, nid, args);
        };
        for (const auto nid : GuestImeKeyboard::Nids) {
            CHECK(GuestImeKeyboard::Admits(nid, "#libSceIme#1#libSceIme#Function"));
            CHECK(!GuestImeKeyboard::Admits(nid, "#libSceImeDialog#1#libSceImeDialog#Function"));
            CHECK(!GuestImeKeyboard::Admits(nid, "#libSceIme#2#libSceIme#Function"));
        }
        CHECK(!GuestImeKeyboard::IsNid("RPydv-Jr1bc")); // software IME callback UI is separate
        CHECK(!GuestImeKeyboard::IsNid("ua+13Hk9kKs")); // don't admit desktop SetMode stub
        OrbisImeKeyboardParam param{};
        param.handler = reinterpret_cast<OrbisImeEventHandler>(base + 0x4000);
        param.arg = reinterpret_cast<void*>(UINT64_MAX); // opaque, never dereferenced
        const u64 input = base + 0x101, output = base + 0x301;
        auto put = [&] { CHECK(space->WriteData({input}, std::as_bytes(std::span{&param, 1}))); };
        auto open = [&] { put(); return call("eaFXjfJv3xs", {1000, input}); };
        CHECK(call("-4GCfYdNF1s") == u32(ORBIS_IME_ERROR_NOT_OPENED));
        CHECK(call("PMVehSlfZ94", {1000}) == u32(ORBIS_IME_ERROR_NOT_OPENED));
        for (const u64 bad : {u64{0}, base + 0x8000, UINT64_MAX - 7})
            CHECK(call("eaFXjfJv3xs", {1000, bad}) == u32(ORBIS_IME_ERROR_INVALID_ADDRESS));
        param.handler = nullptr;
        CHECK(open() == u32(ORBIS_IME_ERROR_INVALID_HANDLER));
        param.handler = reinterpret_cast<OrbisImeEventHandler>(base);
        CHECK(open() == u32(ORBIS_IME_ERROR_INVALID_HANDLER));
        param.handler = reinterpret_cast<OrbisImeEventHandler>(base + 0x4000);
        param.option = static_cast<OrbisImeKeyboardOption>(0x80000000U);
        CHECK(open() == u32(ORBIS_IME_ERROR_INVALID_OPTION));
        param.option = OrbisImeKeyboardOption::Default;
        param.reserved1[0] = 1;
        CHECK(open() == u32(ORBIS_IME_ERROR_INVALID_RESERVED));
        param.reserved1[0] = 0; param.reserved2[7] = 1;
        CHECK(open() == u32(ORBIS_IME_ERROR_INVALID_RESERVED));
        param.reserved2[7] = 0; put();
        CHECK(call("eaFXjfJv3xs", {u32(-1), input}) == u32(ORBIS_IME_ERROR_INVALID_USER_ID));
        auto enumerate = [&] {
            OrbisImeKeyboardResourceIdArray ids{42, {1, 2, 3, 4, 5}};
            CHECK(space->WriteData({output}, std::as_bytes(std::span{&ids, 1})));
            const auto code = call("dKadqZFgKKQ", {1000, output});
            CHECK(space->ReadData({output}, std::as_writable_bytes(std::span{&ids, 1})));
            CHECK(ids.user_id == 1000 && std::ranges::all_of(ids.resource_id, [](auto id) { return id == 0; }));
            return code;
        };
        CHECK(enumerate() == u32(ORBIS_IME_ERROR_NOT_OPENED));
        CHECK(open() == 0);
        CHECK(open() == u32(ORBIS_IME_ERROR_BUSY));
        CHECK(u32(sceImeKeyboardOpen(1000, &param)) == 0);
        CHECK(u32(sceImeKeyboardOpen(1000, &param)) == u32(ORBIS_IME_ERROR_BUSY));
        CHECK(enumerate() == u32(ORBIS_IME_ERROR_CONNECTION_FAILED));
        OrbisImeKeyboardResourceIdArray native_ids{};
        CHECK(u32(sceImeKeyboardGetResourceId(1000, &native_ids)) == u32(ORBIS_IME_ERROR_CONNECTION_FAILED));
        CHECK(call("-4GCfYdNF1s", {UINT64_MAX}) == u32(sceImeUpdate(nullptr)));
        CHECK(other.Invoke(*space, "-4GCfYdNF1s", {}) == u32(ORBIS_IME_ERROR_NOT_OPENED));
        std::array<u8, 64> sentinel; sentinel.fill(0xa5);
        CHECK(space->WriteData({output}, std::as_bytes(std::span{sentinel})));
        CHECK(call("VkqLPArfFdc", {1, output}) == u32(ORBIS_IME_ERROR_NO_RESOURCE_ID));
        std::array<u8, 64> after{};
        CHECK(space->ReadData({output}, std::as_writable_bytes(std::span{after})) && after == sentinel);
        for (const u64 bad : {u64{0}, base + 0x4000, base + 0x3ff8, UINT64_MAX - 7}) {
            CHECK(call("dKadqZFgKKQ", {1000, bad}) == u32(ORBIS_IME_ERROR_INVALID_ADDRESS));
            CHECK(call("VkqLPArfFdc", {1, bad}) == u32(ORBIS_IME_ERROR_INVALID_ADDRESS));
        }
        CHECK(call("PMVehSlfZ94", {u32(-1)}) == u32(ORBIS_IME_ERROR_INVALID_USER_ID));
        CHECK(call("PMVehSlfZ94", {1000}) == u32(sceImeKeyboardClose(1000)));
        CHECK(open() == 0);
        // Changes to caller storage cannot affect the captured registration.
        param = {}; put();
        CHECK(call("-4GCfYdNF1s") == 0);
        std::thread stop([&] { keyboard.Cancel(); });
        const auto raced = call("-4GCfYdNF1s");
        CHECK(raced == 0 || raced == u32(ORBIS_IME_ERROR_NOT_OPENED));
        stop.join();
        CHECK(call("-4GCfYdNF1s") == u32(ORBIS_IME_ERROR_NOT_OPENED));
        CHECK(call("eaFXjfJv3xs", {1000, input}) == u32(ORBIS_IME_ERROR_NOT_OPENED));
    }
    std::printf("PSVR_SERVICES checks=%u failures=%u\n",checks,failures);
    return failures ? 1 : 0;
}
