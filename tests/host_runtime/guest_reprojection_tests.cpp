// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <future>
#include "core/host_runtime/guest_reprojection.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Hmd;
int main() {
    unsigned checks{}, failures{};
    auto check = [&](const char* name, bool ok) {
        ++checks; failures += !ok;
        if (!ok) std::printf("FAIL %s\n", name);
    };
    AddressSpaceConfig cfg{}; cfg.reservation_size = 4 << 20;
    auto created = GuestAddressSpace::Create(cfg);
    if (!created) return 2;
    auto space = std::move(created).Value();
    const u64 base = space->ReservationBase().value;
    if (!space->Map({GuestAddress{base}, 2 << 20}, GuestPermission::Read | GuestPermission::Write)) return 2;
    auto write = [&](u64 offset, const auto& obj) {
        return bool(space->WriteData(GuestAddress{base + offset}, std::as_bytes(std::span{&obj, 1})));
    };
    std::function<void()> cadence;
    std::vector<std::function<void(bool)>> completions;
    std::vector<ReprojectionFrame> frames;
    unsigned starts{}, ends{};
    GuestReprojection transport(*space, {
        .validate_display = [](s32 v, s32 l, s32 r) { return v == 1 && l == 2 && r == 3 ? 0 : ORBIS_HMD_ERROR_REPROJECTION_WRONG_DISPLAY_BUFFER; },
        .event_exists = [](s64 queue, u32 id) { return queue == 10 && (id == 1 || id == 2); },
        .trigger = [&](s64, u32 id) { if (id == 1) ++starts; else ++ends; },
        .set_cadence = [&](auto cb) { cadence = std::move(cb); },
        .submit = [&](ReprojectionFrame frame, auto cb) { frames.push_back(frame); completions.push_back(std::move(cb)); return 0; }});
    auto call = [&](std::string_view nid, std::array<u64, 6> args = {}, bool enabled = true) {
        return transport.Dispatch(nid, args, enabled);
    };
    check("SBS opt in", call("OuygGEWkins", {}, false) == u32(ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE));
    check("pre-init event validates queue", call("7as0CjXW1B8", {11,1}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("pre-init start event", call("7as0CjXW1B8", {10,1}) == 0);
    check("pre-init end event", call("knyIhlkpLgE", {10,2}) == 0);
    check("duplicate event", call("knyIhlkpLgE", {10,2}) == u32(ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_ALREADY_SET));
    check("display requires init", call("E+dPfjeQLHI", {1,2,3}) == u32(ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED));
    OrbisHmdReprojectionInitializeParam init{};
    init.opaque0 = base+0x10000; init.opaque8 = base+0x20000; init.selector20 = init.selector24 = 5;
    write(0, init);
    check("reserved init argument", call("OuygGEWkins", {base,2,1}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("invalid mode", call("OuygGEWkins", {base,3}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("valid init work buffers", call("OuygGEWkins", {base,2}) == 0);
    check("duplicate init", call("OuygGEWkins", {base,2}) == u32(ORBIS_HMD_ERROR_REPROJECTION_ALREADY_INITIALIZED));
    check("invalid display", call("E+dPfjeQLHI", {1,0,1}) == u32(ORBIS_HMD_ERROR_REPROJECTION_WRONG_DISPLAY_BUFFER));
    check("valid display", call("E+dPfjeQLHI", {1,2,3}) == 0);
    cadence(); check("cadence before first submission", starts == 1 && ends == 0);
    OrbisHmdReprojectionLayer layer{};
    layer.roots00[0] = base+0x400; layer.roots00[1] = base+0x440; layer.root20 = base+0x500;
    AmdGpu::Image eye{}; eye.base_address = 0x400; eye.width = 1919; eye.height = 1079;
    write(0x400, eye); eye.base_array = 1; write(0x440, eye);
    write(0x100, layer);
    OrbisHmdReprojectionSubmission submission{};
    submission.root00 = base+0x600; submission.selector08 = 3000; submission.flags20 = 0x80000005;
    write(0x300, submission);
    const std::array<u64, 6> args{base+0x100,1,base+0x300,base+0x700,73,0};
    layer.kind78 = 1; write(0x100, layer);
    check("unsupported depth layer explicit", call("8gH1aLgty5I", args) == u32(ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE));
    layer.kind78 = 0; write(0x100, layer);
    check("first real submission", call("8gH1aLgty5I", args) == 0);
    check("copied eye identity and sequence", frames.size()==1 && frames[0].eyes[1].base_array==1 && frames[0].sequence==73 && frames[0].display_index==2);
    check("no early completion", ends == 0);
    check("second submission", call("8gH1aLgty5I", args) == 0 && frames[1].display_index == 3);
    check("bounded in-flight", call("8gH1aLgty5I", args) == u32(ORBIS_HMD_ERROR_REPROJECTION_DISPLAY_BUFFER_BUSY));
    completions[0](true); check("retirement completion", ends == 1);
    auto stop = std::async(std::launch::async, [&] { return call("vzMEkwBQciM"); });
    check("stop waits for pending GPU reader", stop.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    completions[1](true); check("stop completes after retirement", stop.get()==0 && ends==2);
    cadence(); check("stopped cadence silent", starts==1);
    check("unset", call("iGNNpDDjcwo")==0);
    check("no stale display submit", call("8gH1aLgty5I", args)==u32(ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER));
    check("finalize", call("ZrV5YIqD09I")==0);
    cadence(); check("finalized cadence silent", starts==1);
    check("reinitialize", call("OuygGEWkins", {base,2})==0);
    check("events cleared by finalize", call("mdyFbaJj66M")==u32(ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_NOT_SET));
    std::printf("guest_reprojection_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
