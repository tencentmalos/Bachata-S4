// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include "core/host_runtime/guest_camera.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Camera;
int main() {
    unsigned checks{}, failures{};
    auto check = [&](const char* name, bool value) {
        ++checks;
        failures += !value;
        if (!value)
            std::printf("FAIL %s\n", name);
    };
    AddressSpaceConfig cfg{};
    cfg.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(cfg);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    if (!space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write))
        return 2;
    unsigned allocations{};
    auto alloc = [&](u64 bytes) {
        ++allocations;
        if (!space->Map({GuestAddress{base + 0x4000}, bytes},
                        GuestPermission::Read | GuestPermission::Write))
            std::abort();
        return base + 0x4000;
    };
    GuestCamera camera(*space, alloc, 0x05000000);
    auto write = [&](const auto& value) {
        return bool(space->WriteData(GuestAddress{base}, std::as_bytes(std::span{&value, 1})));
    };
    auto call = [&](std::string_view nid, std::array<u64, 6> a = {}, bool enabled = true,
                    u64 us = 1234567) { return camera.Dispatch(nid, a, enabled, us); };
    check("ordinary mode has no camera", call("p6n3Npi3YY4", {}, false) == 0);
    check("SBS camera attached", call("p6n3Npi3YY4") == 1);
    check("invalid device index", call("p6n3Npi3YY4", {1}) == u32(ORBIS_CAMERA_ERROR_PARAM));
    check("invalid user", call("BHn83xrF92E", {1000}) == u32(ORBIS_CAMERA_ERROR_PARAM));
    check("ordinary open disconnected",
          call("BHn83xrF92E", {255}, false) == u32(ORBIS_CAMERA_ERROR_NOT_CONNECTED));
    const auto handle = call("BHn83xrF92E", {255});
    check("owned pool open", handle == 1 && allocations == 1);
    check("duplicate open", call("BHn83xrF92E", {255}) == u32(ORBIS_CAMERA_ERROR_ALREADY_OPEN));
    OrbisCameraFrameData frame{};
    frame.sizeThis = sizeof(frame);
    write(frame);
    check("frame before start",
          call("mxgMmR+1Kr0", {handle, base}) == u32(ORBIS_CAMERA_ERROR_NOT_START));
    OrbisCameraConfig config{};
    config.sizeThis = sizeof(config);
    config.configType = ORBIS_CAMERA_CONFIG_TYPE4;
    write(config);
    check("desktop type4 config", call("VQ+5kAqsE2Q", {handle, base}) == 0);
    config.configType = ORBIS_CAMERA_CONFIG_EXTENTION;
    config.configExtention[0].pBaseOption = (void*)1;
    write(config);
    check("nested native pointer rejected",
          call("9+SNhbctk64", {handle, base}) == u32(ORBIS_CAMERA_ERROR_PARAM));
    OrbisCameraVideoSyncParameter sync{};
    sync.sizeThis = sizeof(sync);
    write(sync);
    check("video sync alias", call("8WtmqmE4edw", {handle, base}) == 0);
    OrbisCameraStartParameter start{};
    start.sizeThis = sizeof(start);
    start.formatLevel[0] = 16;
    write(start);
    check("invalid level bits",
          call("9EpRYMy7rHU", {handle, base}) == u32(ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN));
    start.formatLevel[0] = start.formatLevel[1] = 15;
    write(start);
    check("start all eight planes", call("cLxF1QtHch0", {handle, base}) == 0);
    check("duplicate start",
          call("9EpRYMy7rHU", {handle, base}) == u32(ORBIS_CAMERA_ERROR_ALREADY_START));
    frame = {};
    frame.sizeThis = sizeof(frame);
    write(frame);
    check("fixed frame", call("mxgMmR+1Kr0", {handle, base}) == 0);
    check("read frame record",
          bool(space->ReadData(GuestAddress{base}, std::as_writable_bytes(std::span{&frame, 1}))));
    check("frame metadata", frame.meta.frame[0] == 1 && frame.meta.timestamp[0] == 1234567 &&
                                frame.status[0] == 0 && frame.status[1] == 0);
    for (unsigned d = 0; d < 2; ++d)
        for (unsigned l = 0; l < 4; ++l) {
            check("plane size", frame.frameSize[d][l] == (1280u >> l) * (800u >> l) * 2);
            check("plane dimensions", frame.framePosition[d][l].xSize == (1280u >> l) &&
                                          frame.framePosition[d][l].ySize == (800u >> l));
            check("garlic is guest owned",
                  frame.pFramePointerListGarlic[d][l] == frame.pFramePointerList[d][l] &&
                      space->ValidateRange(
                          {GuestAddress{reinterpret_cast<u64>(frame.pFramePointerList[d][l])},
                           frame.frameSize[d][l]},
                          GuestPermission::Read));
            u32 pixel{};
            check("read pixel",
                  bool(space->ReadData(
                      GuestAddress{reinterpret_cast<u64>(frame.pFramePointerList[d][l])},
                      std::as_writable_bytes(std::span{&pixel, 1}))));
            check("default pixel value", pixel == (l ? 0x80108010u : 0));
        }
    check("frame validity", call("U3BVwQl2R5Q", {handle, base}) == 1);
    check("invalid output range",
          call("mxgMmR+1Kr0", {handle, 1}) == u32(ORBIS_CAMERA_ERROR_PARAM));
    std::array<unsigned char, 600> older;
    older.fill(0x5a);
    u32 old_size = 520;
    std::memcpy(older.data(), &old_size, 4);
    write(older);
    check("legacy frame output", call("mxgMmR+1Kr0", {handle, base}, true, 1235000) == 0);
    check("read legacy record",
          bool(space->ReadData(GuestAddress{base}, std::as_writable_bytes(std::span{older}))));
    check("legacy tail untouched",
          std::all_of(older.begin() + 520, older.end(), [](auto x) { return x == 0x5a; }));
    std::memcpy(&frame, older.data(), 520);
    check("monotonic frame metadata",
          frame.meta.frame[1] == 2 && frame.meta.timestamp[1] == 1235000);
    check("stop alias", call("+X1Kgnn3bzg", {handle}) == 0);
    check("stopped frame refused",
          call("mxgMmR+1Kr0", {handle, base}) == u32(ORBIS_CAMERA_ERROR_NOT_START));
    check("close during provider disable", call("ztqH5qNTpTk", {handle}, false) == 0);
    check("closed handle refused",
          call("2G2C0nmd++M", {handle}) == u32(ORBIS_CAMERA_ERROR_NOT_OPEN));
    auto second = call("BHn83xrF92E", {255});
    check("reopen fresh handle stable pool", second == 2 && allocations == 1);
    check("stale handle rejected",
          call("2G2C0nmd++M", {handle}) == u32(ORBIS_CAMERA_ERROR_NOT_OPEN));
    check("close current handle", call("OMS9LlcrvBo", {second}) == 0);
    check(
        "GPU-readable buffer lifetime spans close",
        bool(space->ValidateRange({GuestAddress{base + 0x4000}, 4096000}, GuestPermission::Read)));
    std::printf("GUEST_CAMERA checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
