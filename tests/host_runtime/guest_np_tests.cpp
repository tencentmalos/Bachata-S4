// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <thread>
#include "core/host_runtime/guest_np.h"
#include "core/host_runtime/guest_np_auth.h"
#include "core/host_runtime/guest_np_score.h"
#include "core/host_runtime/guest_np_tus.h"
#include "core/libraries/np/np_score/np_score.h"
#include "core/libraries/np/np_tus/np_tus.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Np;
using namespace Libraries::Np::NpManager;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__);                             \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{base + 0x4000}, 0x4000}, GuestPermission::Read));
    constexpr u64 invalid_user = u32(-1);
    const u64 output = base + 256;
    std::array<u8, 64> sentinel;
    sentinel.fill(0xa5);
    auto reset = [&] {
        CHECK(space->Write(GuestAddress{output}, std::as_bytes(std::span{sentinel})));
    };
    auto bytes = [&] {
        std::array<u8, 64> value{};
        CHECK(space->Read(GuestAddress{output}, std::as_writable_bytes(std::span{value})));
        return value;
    };
    auto unchanged = [&] { CHECK(bytes() == sentinel); };
    auto prefix = [&](const auto& expected) {
        auto want = sentinel;
        std::memcpy(want.data(), &expected, sizeof(expected));
        CHECK(bytes() == want); // also checks surrounding bytes and structure padding
    };
    GuestNpOffline current(Common::ElfInfo::FW_900, {{1000, false}, {1001, true}});
    auto call = [&](std::string_view nid, u64 user = 1000, u64 out = 0) {
        return current.Dispatch(*space, nid, {user, out ? out : output});
    };
    constexpr u32 bad = ORBIS_NP_ERROR_INVALID_ARGUMENT, offline = ORBIS_NP_ERROR_SIGNED_OUT;
    reset();
    CHECK(call("JT+t00a3TxA", 1000) == offline);
    CHECK(call("JT+t00a3TxA", 1001) == offline);
    CHECK(call("JT+t00a3TxA", 1002) == u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
    CHECK(call("JT+t00a3TxA", invalid_user) == bad);
    unchanged();
    for (auto nid : NpOfflineNids) {
        CHECK(AdmitsNpOffline(nid, "#libSceNpManager#1#libSceNpManager#Function", true));
        CHECK(!AdmitsNpOffline(nid, "#libSceNpManager#1#libSceNpManager#Function", false));
        CHECK(!AdmitsNpOffline(nid, "#libkernel#1#libkernel#Function", true));
        CHECK(!AdmitsNpOffline(nid, "#libSceNpManager#2#libSceNpManager#Function", true));
        if (nid == "3Zl8BePTh9Y" || nid == "JELHf4xPufo" || nid == "A2CQ3kgSopQ" || nid == "Ec63y59l9tw")
            continue;
        for (auto ptr : {u64{0}, u64{1}, base + 0x4000, base + 0x8000, UINT64_MAX})
            CHECK(current.Dispatch(*space, nid, {1000, ptr}) == bad);
    }
    struct Restriction { u64 size; s8 age; u8 pad[3]; s32 count; u64 entries; } restriction{24, 18, {}, 0, 0};
    auto restriction_call = [&] {
        CHECK(space->WriteData(GuestAddress{base}, std::as_bytes(std::span{&restriction, 1})));
        return current.Dispatch(*space, "A2CQ3kgSopQ", {base});
    };
    CHECK(restriction_call() == 0);
    restriction.size = 16;
    CHECK(restriction_call() == u32(ORBIS_NP_ERROR_INVALID_SIZE));
    restriction.size = 24; restriction.count = -1;
    CHECK(restriction_call() == bad);
    restriction.count = 257;
    CHECK(restriction_call() == bad);
    restriction.count = 1;
    CHECK(restriction_call() == bad);
    restriction.entries = base + 512;
    CHECK(restriction_call() == 0);
    restriction.age = -1;
    CHECK(restriction_call() == bad);
    CHECK(current.Dispatch(*space, "A2CQ3kgSopQ", {base + 0x8000 - 8}) == bad);
    CHECK(current.Dispatch(*space, "Ec63y59l9tw", {base, base + 1024}) == 0);
    CHECK(current.Dispatch(*space, "Ec63y59l9tw", {base, 1}) == bad);
    CHECK(current.Dispatch(*space, "Ec63y59l9tw", {0, base}) == bad);
    CHECK(IsNpOfflineNid("qQJfO8HAiaY")); // translated local callback registration
    CHECK(IsNpOfflineNid("8Z2Jc5GvGDI")); // desktop offline request family now admitted
    CHECK(
        !AdmitsNpOffline("rbknaUjpqWo", "#libSceNpManagerCompat#1#libSceNpManager#Function", true));
    CHECK(
        AdmitsNpOffline("a8R9-75u4iM", "#libSceNpManagerCompat#1#libSceNpManager#Function", true));
    CHECK(call("3Zl8BePTh9Y") == 0 && call("JELHf4xPufo") == 0);
    reset();
    CHECK(call("rbknaUjpqWo") == offline);
    prefix(u64{0});
    reset();
    CHECK(call("rbknaUjpqWo", invalid_user) == bad);
    unchanged();
    reset();
    CHECK(call("rbknaUjpqWo", 0x12345678) == offline);
    prefix(u64{0}); // no existence check
    const u64 tail = base + 0x4000 - 4;
    CHECK(space->Write(GuestAddress{tail}, std::as_bytes(std::span{sentinel}).first(4)));
    CHECK(call("rbknaUjpqWo", 1000, tail) == bad);
    u32 tail_value{};
    CHECK(space->Read(GuestAddress{tail}, std::as_writable_bytes(std::span{&tail_value, 1})));
    CHECK(tail_value == 0xa5a5a5a5); // no partial clear across a read-only boundary
    for (auto nid : {"p-o74CnoNzY", "XDncXQIJUSk"}) {
        reset();
        CHECK(call(nid) == offline);
        unchanged();
        CHECK(call(nid, invalid_user) == bad);
        unchanged();
    }
    reset();
    CHECK(call("eQH7nWPcAgc") == 0);
    prefix(u32{1});
    reset();
    CHECK(call("e-ZuhGEoeC4") == 0);
    prefix(u32{0});
    reset();
    CHECK(call("oPO9U42YpgI") == 0);
    prefix(u32{0});
    for (auto nid : {"eQH7nWPcAgc", "e-ZuhGEoeC4", "oPO9U42YpgI"}) {
        reset();
        CHECK(call(nid, invalid_user) == bad);
        unchanged();
    }
    reset();
    CHECK(call("VgYczPGB5ss", 1) == offline);
    unchanged();
    CHECK(call("VgYczPGB5ss", 0) == bad);
    unchanged();
    OrbisNpOnlineId online{};
    std::memcpy(online.data, "test-user", 9);
    CHECK(space->Write(GuestAddress{base}, std::as_bytes(std::span{&online, 1})));
    reset();
    CHECK(call("F6E4ycq9Dbg", base) == offline);
    unchanged();
    CHECK(call("F6E4ycq9Dbg", base + 0x8000 - 10) == bad);
    unchanged();
    reset();
    CHECK(call("a8R9-75u4iM", base) == u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
    prefix(u64{0});
    reset();
    CHECK(call("IPb1hd1wAGc", base) == 0);
    prefix(u32{0});
    reset();
    CHECK(call("Oad3rvY-NJQ", 1000) == 0);
    prefix(false);
    reset();
    CHECK(call("Oad3rvY-NJQ", 1001) == 0);
    prefix(true);
    reset();
    CHECK(call("Oad3rvY-NJQ", 2000) == u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
    prefix(false);
    reset();
    CHECK(call("Oad3rvY-NJQ", invalid_user) == bad);
    prefix(false);
    for (s32 sdk : {-1, 0, s32(Common::ElfInfo::FW_400 - 1), s32(Common::ElfInfo::FW_400),
                    s32(Common::ElfInfo::FW_900 - 1), s32(Common::ElfInfo::FW_900)}) {
        GuestNpOffline np(sdk, {});
        const bool modern9 = sdk < 0 || sdk >= s32(Common::ElfInfo::FW_900);
        const bool modern4 = sdk < 0 || sdk >= s32(Common::ElfInfo::FW_400);
        reset();
        CHECK(np.Dispatch(*space, "eQH7nWPcAgc", {invalid_user, output}) == (modern9 ? bad : 0));
        if (modern9)
            unchanged();
        else
            prefix(u32{1});
        reset();
        CHECK(np.Dispatch(*space, "e-ZuhGEoeC4", {invalid_user, output}) == (modern4 ? bad : 0));
        if (modern4)
            unchanged();
        else
            prefix(u32{0});
        for (auto nid : {"p-o74CnoNzY", "XDncXQIJUSk"}) {
            reset();
            CHECK(np.Dispatch(*space, nid, {invalid_user, output}) ==
                  (modern9 ? bad : u32(ORBIS_NP_ERROR_USER_NOT_FOUND)));
            unchanged();
            CHECK(np.Dispatch(*space, nid, {invalid_user, 0}) ==
                  (modern9 ? bad : u32(ORBIS_NP_ERROR_USER_NOT_FOUND)));
        }
        reset();
        CHECK(np.Dispatch(*space, "Oad3rvY-NJQ", {1001, output}) ==
              u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
        prefix(false); // session isolation
    }
    // Desktop request state and guest ownership/ABI are exercised together.
    GuestNpControl control, other;
    auto ctl=[&](std::string_view nid,std::array<u64,6> a={}) { return control.Dispatch(*space,nid,a); };
    OrbisNpCreateAsyncRequestParameter parameter{sizeof(parameter),0xff,700,{}};
    CHECK(space->WriteData(GuestAddress{base+1024},std::as_bytes(std::span{&parameter,1})));
    CHECK(ctl("eiqMCt9UshI",{1})==bad);
    const u64 request=ctl("eiqMCt9UshI",{base+1024}); CHECK(s32(request)>0);
    CHECK(other.Dispatch(*space,"S7QTn72PrDw",{request})==u32(ORBIS_NP_ERROR_REQUEST_NOT_FOUND));
    CHECK(ctl("uqcPJLWL08M",{request,output})==u32(ORBIS_NP_ERROR_INVALID_ID));
    CHECK(ctl("8Z2Jc5GvGDI",{request,1000})==0);
    CHECK(ctl("uqcPJLWL08M",{request,1})==bad);
    CHECK(ctl("uqcPJLWL08M",{request,output})==0);
    s32 request_result{}; CHECK(space->ReadData(GuestAddress{output},std::as_writable_bytes(std::span{&request_result,1})));
    CHECK(u32(request_result)==u32(ORBIS_NP_ERROR_SIGNED_OUT));
    CHECK(ctl("S7QTn72PrDw",{request})==0);
    CHECK(ctl("jyi5p9XWUSs",{request,output})==u32(ORBIS_NP_ERROR_REQUEST_NOT_FOUND));
    CHECK(space->Map({GuestAddress{base+0x8000},0x4000},GuestPermission::Read|GuestPermission::Execute));
    CHECK(IsNpControlNid("KswxLxk4c1Y") && IsNpControlNid("aJZyCcHxzu4"));
    for (auto invalid : {u64(0), base, base + 0x10000, UINT64_MAX})
        CHECK(ctl("KswxLxk4c1Y", {invalid, 123}) == bad);
    std::array<u64, 8> presence_ids{};
    for (auto& id : presence_ids) {
        id = ctl("KswxLxk4c1Y", {base + 0x8000, 123});
        CHECK(s32(id) > 0); // duplicate callback function is a distinct registration
    }
    CHECK(ctl("KswxLxk4c1Y", {base + 0x8000, 123}) == u32(ORBIS_NP_ERROR_CALLBACK_MAX));
    {
        GuestNpControl other_presence;
        CHECK(other_presence.Dispatch(*space, "aJZyCcHxzu4", {presence_ids[0]}) ==
              u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    }
    CHECK(ctl("aJZyCcHxzu4", {presence_ids[0]}) == 0);
    CHECK(ctl("aJZyCcHxzu4", {presence_ids[0]}) == u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    const auto replacement_presence = ctl("KswxLxk4c1Y", {base + 0x8000, 456});
    CHECK(s32(replacement_presence) > 0 && replacement_presence != presence_ids[0]);
    CHECK(ctl("aJZyCcHxzu4", {replacement_presence}) == 0);
    for (size_t i = 1; i < presence_ids.size(); ++i)
        CHECK(ctl("aJZyCcHxzu4", {presence_ids[i]}) == 0);
    {
        GuestNpControl temporary;
        for (size_t i = 0; i < 8; ++i)
            CHECK(s32(temporary.Dispatch(*space, "KswxLxk4c1Y", {base + 0x8000, i})) > 0);
    } // Destructor must release the native registration slots.
    const auto after_presence_cleanup = ctl("KswxLxk4c1Y", {base + 0x8000, 789});
    CHECK(s32(after_presence_cleanup) > 0);
    CHECK(ctl("aJZyCcHxzu4", {after_presence_cleanup}) == 0);
    CHECK(ctl("VfRSmPmj8Q8",{base,123})==bad);
    CHECK(ctl("VfRSmPmj8Q8",{base+0x8000,123})==0);
    CHECK(ctl("VfRSmPmj8Q8",{base+0x8000,456})==u32(ORBIS_NP_ERROR_CALLBACK_ALREADY_REGISTERED));
    NotifyNpStateFromUserServiceEvent(Libraries::UserService::OrbisUserServiceEventType::Login,1000);
    auto callbacks=control.BeginCallbacks(); CHECK(callbacks && callbacks->size()==1);
    CHECK(!control.BeginCallbacks());
    if (callbacks && callbacks->size()==1) {
        const auto cb=callbacks->front();
        CHECK(cb.user==1000 && cb.state==u32(OrbisNpState::SignedOut) && cb.argument==123 && !cb.unsupported_identity);
        CHECK(control.IsCurrent(cb));
        CHECK(ctl("mjjTXh+NHWY")==0);
        CHECK(!control.IsCurrent(cb));
    }
    control.EndCallbacks();
    callbacks=control.BeginCallbacks(); CHECK(callbacks && callbacks->empty()); control.EndCallbacks();
    CHECK(ctl("GImICnh+boA",{base+0x8000,987})==0);
    CHECK(ctl("xViqJdDgKl0")==0);
    CHECK(ctl("xViqJdDgKl0")==u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    CHECK(AdmitsNpOffline("0c7HbXRKUt4","#libSceNpManagerForToolkit#1#libSceNpManager#Function",true));
    CHECK(ctl("0c7HbXRKUt4",{base,123})==bad);
    CHECK(ctl("0c7HbXRKUt4",{base+0x8000,321})==0);
    NotifyNpStateFromUserServiceEvent(Libraries::UserService::OrbisUserServiceEventType::Login,1001);
    callbacks=control.BeginCallbacks(); CHECK(callbacks && callbacks->size()==1);
    if (callbacks && callbacks->size()==1) {
        const auto cb=callbacks->front();
        CHECK(cb.kind == GuestNpControl::CallbackKind::Toolkit && cb.argument==321 && cb.user==1001 && cb.state==u32(OrbisNpState::SignedOut));
        CHECK(control.IsCurrent(cb));
        CHECK(ctl("YIvqqvJyjEc")==0);
        CHECK(!control.IsCurrent(cb));
    }
    control.EndCallbacks();
    CHECK(ctl("YIvqqvJyjEc")==u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    // Multiple StateA registrations retain native ids, distinct guest userdata,
    // and revision checks when an id/function is reused during callback dispatch.
    CHECK(ctl("qQJfO8HAiaY", {base, 1}) == bad);
    CHECK(ctl("hw5KNqAAels", {0, 1}) == bad);
    std::array<u64, 8> state_ids{};
    for (size_t i = 0; i < state_ids.size(); ++i) {
        state_ids[i] = ctl("qQJfO8HAiaY", {base + 0x8000 + i, 200 + i});
        CHECK(s32(state_ids[i]) > 0);
    }
    CHECK(ctl("qQJfO8HAiaY", {base + 0x8000, 999}) == u32(ORBIS_NP_ERROR_CALLBACK_ALREADY_REGISTERED));
    CHECK(ctl("qQJfO8HAiaY", {base + 0x8100, 999}) == u32(ORBIS_NP_ERROR_CALLBACK_MAX));
    CHECK(ctl("hw5KNqAAels", {base + 0x8200, 333}) == 0);
    CHECK(ctl("hw5KNqAAels", {base + 0x8201, 444}) == u32(ORBIS_NP_ERROR_CALLBACK_ALREADY_REGISTERED));
    CHECK(other.Dispatch(*space, "M3wFXbYQtAA", {state_ids[0]}) == u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    CHECK(other.Dispatch(*space, "cRILAEvn+9M", {}) == u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    NotifyNpStateFromUserServiceEvent(Libraries::UserService::OrbisUserServiceEventType::Login, 1002);
    callbacks = control.BeginCallbacks();
    CHECK(callbacks && callbacks->size() == 9);
    if (callbacks && callbacks->size() == 9) {
        for (size_t i = 0; i < 8; ++i) {
            const auto& cb = (*callbacks)[i];
            CHECK(cb.kind == GuestNpControl::CallbackKind::StateA && cb.index == i &&
                  cb.user == 1002 && cb.state == u32(OrbisNpState::SignedOut) &&
                  cb.function == base + 0x8000 + i && cb.argument == 200 + i && control.IsCurrent(cb));
        }
        const auto& reach = callbacks->back();
        CHECK(reach.kind == GuestNpControl::CallbackKind::Reachability && reach.argument == 333 &&
              reach.state == u32(OrbisNpReachabilityState::Unavailable) && control.IsCurrent(reach));
        CHECK(ctl("M3wFXbYQtAA", {state_ids[0]}) == 0);
        CHECK(ctl("qQJfO8HAiaY", {base + 0x8000, 900}) == state_ids[0]);
        CHECK(!control.IsCurrent(callbacks->front()));
        CHECK(ctl("cRILAEvn+9M") == 0);
        CHECK(ctl("hw5KNqAAels", {base + 0x8200, 555}) == 0);
        CHECK(!control.IsCurrent(reach));
    }
    control.EndCallbacks();
    // First edge after re-registration is delivered; repeated offline states
    // thereafter do not fabricate a reachability transition.
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        NotifyNpStateFromUserServiceEvent(Libraries::UserService::OrbisUserServiceEventType::Login, 1002);
        callbacks = control.BeginCallbacks();
        CHECK(callbacks && callbacks->size() == (repeat == 0 ? 9 : 8));
        control.EndCallbacks();
    }
    CHECK(ctl("M3wFXbYQtAA", {0}) == bad);
    CHECK(ctl("M3wFXbYQtAA", {9}) == bad);
    for (auto id : state_ids) CHECK(ctl("M3wFXbYQtAA", {id}) == 0);
    CHECK(ctl("M3wFXbYQtAA", {state_ids[0]}) == u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    CHECK(ctl("cRILAEvn+9M") == 0);
    CHECK(ctl("cRILAEvn+9M") == u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED));
    {
        GuestNpControl temporary;
        CHECK(s32(temporary.Dispatch(*space, "qQJfO8HAiaY", {base + 0x8000, 123})) > 0);
        CHECK(temporary.Dispatch(*space, "hw5KNqAAels", {base + 0x8200, 333}) == 0);
    }
    CHECK(ctl("qQJfO8HAiaY", {base + 0x8000, 123}) == state_ids[0]);
    CHECK(ctl("M3wFXbYQtAA", {state_ids[0]}) == 0);
    CHECK(ctl("hw5KNqAAels", {base + 0x8200, 333}) == 0);
    CHECK(ctl("cRILAEvn+9M") == 0);
    // Compare the offline subset with desktop, including error precedence and
    // untouched Poll/Wait output. No successful creator is exposed by this family.
    using namespace Libraries::Np::NpScore;
    for (const auto nid : NpScoreOfflineNids) {
        CHECK(AdmitsNpScoreOffline(nid, "#libSceNpScore#1#libSceNpScore#Function", true));
        CHECK(!AdmitsNpScoreOffline(nid, "#libSceNpScore#1#libSceNpScore#Function", false));
        CHECK(!AdmitsNpScoreOffline(nid, "#libSceNpScoreCompat#1#libSceNpScore#Function", true));
        CHECK(!AdmitsNpScoreOffline(nid, "#libkernel#1#libkernel#Function", true));
    }
    for (const s32 user : {-1, 0, 1000, 1001}) {
        for (const u32 label : {0U, 7U, UINT32_MAX}) {
            const auto actual = DispatchNpScoreOffline("GWnWQNXZH5M", {label, u32(user)});
            CHECK(actual == u32(sceNpScoreCreateNpTitleCtxA(label, user)));
            CHECK(actual == u32(ORBIS_NP_ERROR_SIGNED_OUT));
        }
    }
    for (const s32 id : {-1, 0, 1, INT32_MAX}) {
        CHECK(DispatchNpScoreOffline("G0pE+RNCwfk", {u32(id)}) == u32(sceNpScoreDeleteNpTitleCtx(id)));
        CHECK(DispatchNpScoreOffline("gW8qyjYrUbk", {u32(id)}) == u32(sceNpScoreCreateRequest(id)));
        CHECK(DispatchNpScoreOffline("dK8-SgYf6r4", {u32(id)}) == u32(sceNpScoreDeleteRequest(id)));
        CHECK(DispatchNpScoreOffline("1i7kmKbX6hk", {u32(id)}) == u32(sceNpScoreAbortRequest(id)));
        s32 result = 0x12345678;
        const auto poll = u32(sceNpScorePollAsync(id, &result));
        const auto wait = u32(sceNpScoreWaitAsync(id, &result));
        CHECK(poll == u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ID) && wait == poll);
        CHECK(result == 0x12345678);
        reset();
        for (const auto ptr : {u64{0}, u64{1}, output, base + 0x8000, UINT64_MAX}) {
            CHECK(DispatchNpScoreOffline("m1DfNRstkSQ", {u32(id), ptr}) == poll);
            CHECK(DispatchNpScoreOffline("fqk8SC63p1U", {u32(id), ptr}) == wait);
        }
        unchanged();
        for (const s32 pc : {-1, 0, INT32_MAX})
            CHECK(DispatchNpScoreOffline("bygbKdHmjn4", {u32(id), u32(pc)}) ==
                  u32(sceNpScoreSetPlayerCharacterId(id, pc)));
    }
    for (const auto nid : {"KnNA1TEgtBI", "qW9M0bQ-Zx0", "S3xZj35v8Z8", "yxK68584JAU",
                           "KBHxDjyk-jA", "ANJssPz3mY0"})
        CHECK(!IsNpScoreOfflineNid(nid)); // legacy creation, desktop stubs and online requests
    for (const auto nid : NpTusOfflineNids) {
        CHECK(AdmitsNpTusOffline(nid, "#libSceNpTus#1#libSceNpTus#Function", true));
        CHECK(!AdmitsNpTusOffline(nid, "#libSceNpTus#1#libSceNpTus#Function", false));
        CHECK(!AdmitsNpTusOffline(nid, "#libSceNpTusCompat#1#libSceNpTus#Function", true));
        CHECK(!AdmitsNpTusOffline(nid, "#libkernel#1#libkernel#Function", true));
    }
    using namespace Libraries::Np::NpTus;
    for (const s32 sdk : {-1, s32(Common::ElfInfo::FW_400), s32(Common::ElfInfo::FW_900)}) {
        for (const s32 user : {-1, 0, 1000}) {
            for (const u32 label : {0U, UINT32_MAX}) {
                OrbisNpId identity{};
                const auto expected = u32(Offline::Identity(user, &identity, sdk));
                CHECK(DispatchNpTusOffline("1n-dGukBgnY", {label, u32(user)}, sdk) == expected);
                CHECK(DispatchNpTusOffline("lBtrk+7lk14", {label, u32(user)}, sdk) == expected);
            }
        }
    }
    for (const s32 id : {-1, 0, 1, INT32_MAX}) {
        auto tus = [&](std::string_view nid, u64 out = 0) {
            return DispatchNpTusOffline(nid, {u32(id), out}, Common::ElfInfo::FW_900);
        };
        CHECK(tus("H3uq7x0sZOI") == u32(sceNpTusDeleteNpTitleCtx(id)));
        CHECK(tus("3bh2aBvvmvM") == u32(sceNpTusCreateRequest(id)));
        CHECK(tus("CcIH40dYS88") == u32(sceNpTusDeleteRequest(id)));
        CHECK(tus("2eq1bMwgZYo") == u32(sceNpTusAbortRequest(id)));
        s32 result = 0x12345678;
        const auto poll = u32(sceNpTusPollAsync(id, &result));
        const auto wait = u32(sceNpTusWaitAsync(id, &result));
        CHECK(poll == u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ID) && wait == poll);
        CHECK(result == 0x12345678);
        reset();
        for (const auto ptr : {u64{0}, u64{1}, output, base + 0x8000, UINT64_MAX}) {
            CHECK(tus("t7b6dmpQNiI", ptr) == poll);
            CHECK(tus("hYPJFWzFPjA", ptr) == wait);
        }
        unchanged();
    }
    for (const auto nid : {"sRVb2Cf0GHg", "6GKDdRCFx8c", "KMlHj+tgfdQ", "-SUR+UoLS6c"})
        CHECK(!IsNpTusOfflineNid(nid));
    {
        using namespace Libraries::Np::NpAuth;
        constexpr u32 bad_auth = ORBIS_NP_AUTH_ERROR_INVALID_ARGUMENT;
        constexpr u32 missing_auth = ORBIS_NP_AUTH_ERROR_REQUEST_NOT_FOUND;
        GuestNpAuthOffline auth, other;
        auto auth_call = [&](std::string_view nid, std::array<u64, 6> args = {}) {
            return auth.Dispatch(*space, nid, args);
        };
        for (auto nid : GuestNpAuthOffline::Nids) {
            CHECK(GuestNpAuthOffline::Admits(nid, "#libSceNpAuth#1#libSceNpAuth#Function", true));
            CHECK(!GuestNpAuthOffline::Admits(nid, "#libSceNpAuth#1#libSceNpAuth#Function", false));
            CHECK(!GuestNpAuthOffline::Admits(nid, "#libSceNpManager#1#libSceNpManager#Function",
                                              true));
        }
        auto put = [&]<typename T>(u64 address, const T& value) {
            CHECK(space->WriteData({address}, std::as_bytes(std::span{&value, 1})));
        };
        OrbisNpAuthCreateAsyncRequestParameter create{24, 0, 700, {}};
        for (u64 address : {u64{0}, u64{1}, base + 0x8000 - 8, UINT64_MAX})
            CHECK(auth_call("N+mr7GjTvr8", {address}) == bad_auth);
        create.size = 8;
        put(base, create);
        CHECK(auth_call("N+mr7GjTvr8", {base}) == u32(ORBIS_NP_AUTH_ERROR_INVALID_SIZE));
        create.size = 24;
        put(base, create);
        const u32 async = auth_call("N+mr7GjTvr8", {base});
        CHECK(s32(async) > 0);
        reset();
        CHECK(auth_call("gjSyfzSsDcE", {async, output}) == u32(ORBIS_NP_AUTH_ERROR_INVALID_ID));
        unchanged();
        CHECK(other.Dispatch(*space, "H8wG9Bk-nPc", {async}) == missing_auth);
        struct Code {
            u64 size;
            s32 user;
            u32 pad;
            u64 client, scope;
        } code{32, 1000, 0, base + 512, base + 2048};
        struct Token {
            u64 size;
            s32 user;
            u32 pad;
            u64 client, secret, scope;
        } token{40, 1000, 0, base + 512, base + 1024, base + 2048};
        put(base + 64, code);
        put(base + 128, token);
        OrbisNpClientId client{};
        OrbisNpClientSecret secret{};
        put(base + 512, client);
        put(base + 1024, secret);
        const std::array<char, 5> scope{'t', 'e', 's', 't', 0};
        put(base + 2048, scope);
        // Output protection and all-or-nothing admission before mutating the request.
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, base + 0x4000}) == bad_auth);
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, output, base + 0x8000 - 2}) == bad_auth);
        auto invalid_code = code;
        invalid_code.client = UINT64_MAX;
        put(base + 64, invalid_code);
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, output}) == bad_auth);
        invalid_code = code;
        invalid_code.scope = UINT64_MAX;
        put(base + 64, invalid_code);
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, output}) == bad_auth);
        // Use a non-terminated writable scope so it reaches the explicit bound.
        std::array<char, 1025> long_scope;
        long_scope.fill('x');
        put(base + 2048, long_scope);
        put(base + 64, code);
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, output}) == bad_auth);
        put(base + 2048, scope);
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, output, output + 60}) == 0);
        unchanged(); // Async launch succeeds but no credential or issuer is fabricated.
        CHECK(auth_call("gjSyfzSsDcE", {async, output}) == 0);
        prefix(s32(ORBIS_NP_ERROR_SIGNED_OUT));
        CHECK(auth_call("cE7wIsqXdZ8", {async}) == 0); // complete remains signed out
        reset();
        CHECK(auth_call("SK-S7daqJSE", {async, output}) == 0);
        prefix(s32(ORBIS_NP_ERROR_SIGNED_OUT));
        CHECK(auth_call("qAUXQ9GdWp8", {async, base + 64, output}) == bad_auth);
        CHECK(auth_call("H8wG9Bk-nPc", {async}) == 0);
        CHECK(auth_call("H8wG9Bk-nPc", {async}) == missing_auth);
        const u32 aborted = auth_call("N+mr7GjTvr8", {base});
        CHECK(auth_call("cE7wIsqXdZ8", {aborted}) == 0);
        reset();
        CHECK(auth_call("gjSyfzSsDcE", {aborted, output}) == 0);
        prefix(s32(ORBIS_NP_AUTH_ERROR_ABORTED));
        CHECK(auth_call("H8wG9Bk-nPc", {aborted}) == 0);
        // Token has a larger output; ensure offline leaves every byte and its guard unchanged.
        std::array<u8, 4112> token_sentinel;
        token_sentinel.fill(0xa5);
        put(base + 8192, token_sentinel);
        const u32 token_request = auth_call("N+mr7GjTvr8", {base});
        CHECK(auth_call("CocbHVIKPE8", {token_request, base + 128, base + 8192}) == 0);
        std::array<u8, 4112> token_read{};
        CHECK(space->ReadData({base + 8192}, std::as_writable_bytes(std::span{token_read})));
        CHECK(token_read == token_sentinel);
        reset();
        CHECK(auth_call("gjSyfzSsDcE", {token_request, output}) == 0);
        prefix(s32(ORBIS_NP_ERROR_SIGNED_OUT));
        CHECK(auth_call("H8wG9Bk-nPc", {token_request}) == 0);
        const u32 sync = auth_call("6bwFkosYRQg");
        reset();
        CHECK(auth_call("qAUXQ9GdWp8", {sync, base + 64, output}) == offline);
        unchanged();
        CHECK(auth_call("gjSyfzSsDcE", {sync, output}) == u32(ORBIS_NP_AUTH_ERROR_INVALID_ID));
        CHECK(auth_call("H8wG9Bk-nPc", {sync}) == 0);
        for (u64 id : {0ULL, 1ULL, 0xffffffffULL, 0x10000000ULL}) {
            CHECK(auth_call("cE7wIsqXdZ8", {id}) == missing_auth);
            CHECK(auth_call("H8wG9Bk-nPc", {id}) == missing_auth);
        }
        // Native capacity must be atomic even across independently owned domains.
        std::array<s32, 32> ids{};
        std::vector<std::thread> creators;
        for (size_t i = 0; i < ids.size(); ++i)
            creators.emplace_back([&, i] { ids[i] = sceNpAuthCreateRequest(); });
        for (auto& t : creators)
            t.join();
        CHECK(std::ranges::count_if(ids, [](s32 id) { return id > 0; }) == 16);
        CHECK(std::ranges::count(ids, s32(ORBIS_NP_AUTH_ERROR_REQUEST_MAX)) == 16);
        for (auto id : ids)
            if (id > 0)
                CHECK(sceNpAuthDeleteRequest(id) == 0);
        for (int session = 0; session < 32; ++session) {
            GuestNpAuthOffline scoped;
            CHECK(s32(scoped.Dispatch(*space, "6bwFkosYRQg", {})) > 0);
        } // destructor releases every session request
    }
    std::printf("NP offline: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
