// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <future>
#include <limits>
#include "core/host_runtime/guest_reprojection.h"
#include "core/host_runtime/guest_hmd_diagnostics.h"
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
    bool active{};
    GuestReprojection transport(*space, {
        .validate_display = [](s32 v, s32 l, s32 r) { return v == 1 && l == 2 && r == 3 ? 0 : ORBIS_HMD_ERROR_REPROJECTION_WRONG_DISPLAY_BUFFER; },
        .event_exists = [](s64 queue, u32 id) { return queue == 10 && (id == 1 || id == 2); },
        .trigger = [&](s64, u32 id) { if (id == 1) ++starts; else ++ends; },
        .set_cadence = [&](auto cb) { cadence = std::move(cb); },
        .set_active = [&](bool enabled) { active = enabled; },
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
    check("display owns presentation", active);
    cadence(); check("cadence before first submission", starts == 1 && ends == 0);
    OrbisHmdReprojectionLayer layer{};
    layer.roots00[0] = base+0x400; layer.roots00[1] = base+0x440; layer.root20 = base+0x500;
    AmdGpu::Image eye{}; eye.base_address = 0x400; eye.width = 1919; eye.height = 1079;
    write(0x400, eye); eye.base_array = 1; write(0x440, eye);
    const auto& fov = SbsFieldOfView;
    const float tan_uv[8]{1.f/(fov.tan_out+fov.tan_in), 1.f/(fov.tan_top+fov.tan_bottom),
        fov.tan_out/(fov.tan_out+fov.tan_in), .5f,
        1.f/(fov.tan_out+fov.tan_in), 1.f/(fov.tan_top+fov.tan_bottom),
        fov.tan_in/(fov.tan_out+fov.tan_in), .5f};
    std::memcpy(layer.color_tan_to_uv, tan_uv, sizeof(tan_uv));
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
    check("stop releases presentation", !active);
    cadence(); check("stopped cadence silent", starts==1);
    check("unset", call("iGNNpDDjcwo")==0);
    check("no stale display submit", call("8gH1aLgty5I", args)==u32(ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER));
    check("finalize", call("ZrV5YIqD09I")==0);
    cadence(); check("finalized cadence silent", starts==1);
    check("reinitialize", call("OuygGEWkins", {base,2})==0);
    check("events cleared by finalize", call("mdyFbaJj66M")==u32(ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_NOT_SET));
    check("finalize before segmented workspace", call("ZrV5YIqD09I")==0);
    // Tetris: 1 MiB garlic allocation spans two independently mapped pool
    // blocks; neither the ABI nor virtual contiguity requires one mapping.
    const u64 work = base + 0x200000;
    check("map first garlic segment", bool(space->Map({{work}, 0xe0000}, GuestPermission::Read | GuestPermission::Write)));
    check("map second garlic segment", bool(space->Map({{work + 0xe0000}, 0x20000}, GuestPermission::Read | GuestPermission::Write)));
    init.opaque8 = work; init.selector20 = init.selector24 = 3;
    init.field10 = 0x100; init.field18 = 0x3f; init.reserved34 = 0x18;
    write(0, init);
    check("base API still rejects cross-mapping spans", !space->AcquireDataSpan({{work}, 0x100000}, true));
    check("Tetris segmented work buffers", call("OuygGEWkins", {base,2}) == 0);
    check("finalize segmented buffers", call("ZrV5YIqD09I") == 0);
    check("protect final segment", bool(space->Protect({{work+0xe0000},0x20000}, GuestPermission::Read)));
    check("readonly tail rejected", call("OuygGEWkins", {base,2}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("unmap final segment", bool(space->Unmap({{work+0xe0000},0x20000})));
    check("hole rejected", call("OuygGEWkins", {base,2}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("restore tail", bool(space->Map({{work+0xe0000},0x20000}, GuestPermission::Read | GuestPermission::Write)));
    check("failed init did not retain state or pins", call("OuygGEWkins", {base,2}) == 0);
    check("finalize before onion split", call("ZrV5YIqD09I") == 0);
    const u64 onion = work + 0x100000;
    check("map first onion segment", bool(space->Map({{onion},0x4000}, GuestPermission::Read | GuestPermission::Write)));
    check("map second onion segment", bool(space->Map({{onion+0x4000},0x4000}, GuestPermission::Read | GuestPermission::Write)));
    init.opaque0 = onion + 0x3f00; write(0, init);
    check("onion and garlic both cross mappings", call("OuygGEWkins", {base,2}) == 0);
    check("finalize both", call("ZrV5YIqD09I") == 0);
    init.opaque8 = UINT64_MAX - 255; write(0, init);
    check("overflow rejected", call("OuygGEWkins", {base,2}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    init.opaque8 = work; write(0, init);
    check("legacy init", call("OuygGEWkins", {base,2}) == 0);
    check("legacy display", call("E+dPfjeQLHI", {1,2,3}) == 0);
    OrbisHmdReprojectionStartParam legacy{};
    legacy.textures[0] = legacy.textures[1] = base+0x400;
    legacy.sampler = base+0x500;
    legacy.release_label = base+0x600;
    legacy.selector = 3000; legacy.flags = 5;
    std::memcpy(legacy.tan_to_uv, tan_uv, sizeof(tan_uv));
    // Shared dynamic atlas: left [0,.375], right [.375,.75], both height .75.
    for (u32 e=0; e<2; ++e) {
        legacy.tan_to_uv[e][0] *= .375f;
        legacy.tan_to_uv[e][2] = legacy.tan_to_uv[e][2] * .375f + e * .375f;
        legacy.tan_to_uv[e][1] *= .75f; legacy.tan_to_uv[e][3] *= .75f;
    }
    write(0x800, legacy);
    OrbisHmdReprojectionOverlayParam overlay{};
    overlay.textures[0] = base+0x400; overlay.textures[1] = base+0x440;
    overlay.sampler = base+0x500;
    std::memcpy(overlay.tan_to_uv, tan_uv, sizeof(tan_uv));
    write(0x900, overlay);
    const std::array<u64,6> overlay_args{base+0x800,base+0x700,99,base+0x900,0,0};
    check("overlay fifth reserved argument", call("kcldQ7zLYQQ",{base+0x800,base+0x700,99,base+0x900,1}) == u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("overlay null record", call("kcldQ7zLYQQ",{base+0x800,base+0x700,99,0,0}) == u32(ORBIS_HMD_ERROR_PARAMETER_NULL));
    overlay.reserved[8]=1;write(0x900,overlay);
    check("overlay final reserved word",call("kcldQ7zLYQQ",overlay_args)==u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    overlay.reserved[8]=0;write(0x900,overlay);
    legacy.selector=1999;write(0x800,legacy);
    check("legacy selector range",call("kcldQ7zLYQQ",overlay_args)==u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    legacy.selector=2000;write(0x800,legacy);
    check("legacy selector flag combination",call("kcldQ7zLYQQ",overlay_args)==u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    legacy.selector=3000;legacy.tan_to_uv[0][0]=std::numeric_limits<float>::quiet_NaN();write(0x800,legacy);
    check("NaN rejected before renderer",call("kcldQ7zLYQQ",overlay_args)==u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    legacy.tan_to_uv[0][0]=tan_uv[0]*.375f;write(0x800,legacy);
    AmdGpu::Sampler sampler{};sampler.raw1=3ull<<62;write(0x500,sampler);
    check("custom border cannot native-dereference guest table",call("kcldQ7zLYQQ",overlay_args)==u32(ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE));
    sampler={};sampler.raw0=6;write(0x500,sampler);
    const size_t first=frames.size();
    check("five-argument overlay submit",call("kcldQ7zLYQQ",overlay_args)==0);
    check("snapshot contains four textures and sequence",frames.back().image_count==4 && frames.back().sequence==99 && frames.back().eyes[3].base_array==1);
    check("shared atlas UV survives identity",std::abs(frames.back().uv[0][0]-.375f)<1e-6f && std::abs(frames.back().uv[1][2]-.375f)<1e-6f && std::abs(frames.back().uv[0][1]-.75f)<1e-6f);
    check("overlay UV independent of base",std::abs(frames.back().uv[2][0]-1.f)<1e-6f && std::abs(frames.back().uv[3][2])<1e-6f);
    check("sampler snapshot retained",frames.back().samplers[0].clamp_x==AmdGpu::ClampMode::ClampBorder);
    check("four-argument single submit",call("dntZTJ7meIU",{base+0x800,base+0x700,100,0})==0);
    completions[first+1](true);
    check("out-of-order retire can reuse only freed slot",call("dntZTJ7meIU",{base+0x800,base+0x700,101,0})==0 && frames[first+2].display_index==frames[first+1].display_index);
    completions[first+1](true); // stale duplicate must not retire a different submission
    check("duplicate completion cannot release newer work",call("dntZTJ7meIU",{base+0x800,base+0x700,102,0})==u32(ORBIS_HMD_ERROR_REPROJECTION_DISPLAY_BUFFER_BUSY));
    completions[first](true);completions[first+2](true);
    for (u32 eye=0;eye<2;++eye) {
        legacy.tan_to_uv[eye][1]=-tan_uv[eye*4+1];
        legacy.tan_to_uv[eye][3]=1.f-tan_uv[eye*4+3];
    }
    write(0x800,legacy);
    check("negative source Y admitted",call("dntZTJ7meIU",{base+0x800,base+0x700,102,0})==0);
    check("guest Y orientation retained",std::abs(frames.back().uv[0][1]+1.f)<1e-6f && std::abs(frames.back().uv[0][3]-1.f)<1e-6f);
    completions.back()(true);
    // Captured Beat Saber PID 18239, call 1 (2026-09-21): exact float bits,
    // one kind-0 layer, shared atlas, flags 0x80000005. Firmware 11.00
    // +0x1820f..+0x182a4 lowers this case to Start: +28/+38 -> +18/+28.
    // Keep this independent of the analytical FOV-based fixture above.
    const std::array<u32, 8> beat_transform_bits{
        0x3e5655f1, 0xbecab66a, 0x3e8165cf, 0x3f000000,
        0x3e5655f1, 0xbecab66a, 0x3f3f4d19, 0x3f000000};
    OrbisHmdReprojectionLayer beat_layer{};
    beat_layer.roots00[0] = beat_layer.roots00[1] = base + 0x400;
    beat_layer.root20 = base + 0x500;
    std::memcpy(beat_layer.color_tan_to_uv, beat_transform_bits.data(), 32);
    std::memcpy(beat_layer.opaque48, beat_transform_bits.data(), 32);
    OrbisHmdReprojectionSubmission beat_submission{};
    beat_submission.root00 = base + 0x600;
    beat_submission.selector08 = 3000;
    beat_submission.flags20 = 0x80000005;
    write(0x100, beat_layer); write(0x200, beat_submission);
    check("captured Beat Multilayer accepted",
          call("8gH1aLgty5I", {base+0x100,1,base+0x200,base+0x700,8191,0})==0);
    const auto beat_frame = frames.back();
    completions.back()(true);
    check("captured Beat atlas has distinct left/right regions",
          beat_frame.image_count==2 && std::abs(beat_frame.uv[0][0]-.5f)<1e-6f &&
          std::abs(beat_frame.uv[0][2])<1e-6f && std::abs(beat_frame.uv[1][2]-.5f)<1e-6f &&
          beat_frame.uv[0][1]==-1.f && beat_frame.uv[1][3]==1.f);
    OrbisHmdReprojectionStartParam lowered{};
    // Byte ranges independently recovered from firmware, not production helpers.
    std::memcpy(&lowered, &beat_layer, 16);
    lowered.sampler=beat_layer.root20;
    std::memcpy(lowered.tan_to_uv, reinterpret_cast<const u8*>(&beat_layer)+0x28, 32);
    lowered.release_label=beat_submission.root00;
    lowered.selector=beat_submission.selector08;
    lowered.flags=beat_submission.flags20;
    write(0xb00,lowered);
    check("firmware-lowered Beat Start accepted",call("dntZTJ7meIU",{base+0xb00,base+0x700,8191,0})==0);
    check("firmware lowering preserves colour source and UV",
          frames.back().uv==beat_frame.uv && frames.back().sequence==beat_frame.sequence &&
          std::memcmp(frames.back().eyes.data(),beat_frame.eyes.data(),sizeof(beat_frame.eyes))==0 &&
          std::memcmp(frames.back().samplers.data(),beat_frame.samplers.data(),sizeof(beat_frame.samplers))==0);
    completions.back()(true);
    // The other float4 pair must not silently replace the colour pair.
    std::memset(beat_layer.opaque48,0xff,sizeof(beat_layer.opaque48));write(0x100,beat_layer);
    check("kind-0 colour does not consume alternate transform pair",
          call("8gH1aLgty5I",{base+0x100,1,base+0x200,base+0x700,8192,0})==0 &&
          frames.back().uv==beat_frame.uv);
    completions.back()(true);
    OrbisHmdReprojection2dParam mono{};
    mono.texture=base+0x400;mono.sampler=base+0x500;mono.uv[0]=.75f;mono.uv[1]=.5f;mono.uv[2]=.125f;mono.release_label=base+0x600;mono.selector=4000;
    write(0xa00,mono);
    check("2D third reserved argument",call("q3e8+nEguyE",{base+0xa00,103,1})==u32(ORBIS_HMD_ERROR_PARAMETER_INVALID));
    check("2D mono submit",call("q3e8+nEguyE",{base+0xa00,103,0})==0);
    check("2D duplicates image with explicit same rectangle",frames.back().image_count==2 && frames.back().eyes[0].base_address==frames.back().eyes[1].base_address && frames.back().uv[0]==frames.back().uv[1] && frames.back().uv[0][2]==.125f);
    completions.back()(true);
    check("legacy finalize",call("ZrV5YIqD09I")==0);
    // Mapping generation captured before publication must reject replacement.
    GuestDataBatch stale;
    check("snapshot image mapping",stale.Add(*space,{{work+0xe0000},0x20000},GuestPermission::Read));
    check("retire snapshot mapping",bool(space->Unmap({{work+0xe0000},0x20000})));
    check("replace same address",bool(space->Map({{work+0xe0000},0x20000},GuestPermission::Read|GuestPermission::Write)));
    check("same VA different mapping rejected",!stale.Acquire(*space));
    // Diagnostics must not change memory or invent successful output records.
    unsigned samples{};
    for (u64 i = 0; i < 131073; ++i) samples += HmdDiagnostics::Sample(i);
    check("HMD log bounded per-entry sample budget", samples == 28);
    auto trace = HmdDiagnostics::Records(*space, "sceHmdReprojectionStartWithOverlay", overlay_args, false);
    check("HMD log includes independent nested eye pairs", trace.size() == 9);
    check("HMD log preserves complete base record", trace[0]["le_hex"] == HmdDiagnostics::Bytes(legacy));
    check("HMD log retains signed UV", trace[0]["tan_to_uv"][0][1].get<float>() < 0);
    check("HMD failure does not read undefined outputs",
          HmdDiagnostics::Records(*space,"sceHmdGetFieldOfView",{},true,0x80).empty());
    auto bad_trace = HmdDiagnostics::Records(*space,"sceHmdGetFieldOfView",{0,UINT64_MAX},true);
    check("HMD invalid pointer reported unreadable",bad_trace.size()==1 && !bad_trace[0]["readable"].get<bool>() && !bad_trace[0].contains("le_hex"));
    write(0xc00,SbsFieldOfView);
    auto fov_trace = HmdDiagnostics::Records(*space,"sceHmdGetFieldOfView",{0,base+0xc00},true);
    check("HMD exact FOV output bytes",fov_trace[0]["le_hex"]==HmdDiagnostics::Bytes(SbsFieldOfView));
    OrbisHmdFieldOfView unchanged{};
    const bool read_back=bool(space->ReadData({base+0xc00},std::as_writable_bytes(std::span{&unchanged,1})));
    check("HMD diagnostics leave guest memory unchanged",read_back && std::memcmp(&unchanged,&SbsFieldOfView,sizeof(unchanged))==0);
    const auto frame_trace=HmdDiagnostics::Frame(frames.back());
    check("HMD decoded submit preserves per-eye coordinates",frame_trace[1]["uv_scale_bias"]==frames.back().uv[1]);
    std::printf("guest_reprojection_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
