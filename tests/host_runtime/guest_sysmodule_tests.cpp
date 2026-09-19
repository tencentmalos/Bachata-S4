// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include "core/host_runtime/guest_sysmodule_hle.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
int main() {
    unsigned checks{}, failures{};
    auto check = [&](const char* name, bool ok) {
        ++checks;
        failures += !ok;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
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
    auto set = [&](u64 addr, u32 value) {
        return bool(space->Write(GuestAddress{addr}, std::as_bytes(std::span{&value, 1})));
    };
    auto get = [&](u64 addr) {
        u32 value{};
        if (!space->Read(GuestAddress{addr}, std::as_writable_bytes(std::span{&value, 1})))
            std::abort();
        return value;
    };
    GuestSysmodules modules([](u32 id) -> std::optional<std::string> {
        if (id == 0xd7)
            return "libSceDiscMap";
        if (id == 0xab)
            return "missing";
        if (id == 0xe7)
            return "libSceJson2";
        if (id == 0x8f) return "libSceMove";
        if (id == 0xd4) return "libSceHmd";
        if (id == 0xed) return "libSceVrTracker";
        if (id == 0x8000001a) return "libSceCamera";
        if (id == 0xeb) return "libSceHmdSetupDialog";
        return {};
    });
    modules.Publish("libSceDiscMap", 31);
    // Late SBS availability must preserve real providers and API reference
    // visibility, including the u32 Camera internal ID and unload/reload.
    modules.Publish("libSceCamera", 99);
    modules.PublishSbsVrProviders(false);
    check("flat mode does not publish HMD", modules.Load(0xd4) == ORBIS_SYSMODULE_LOCK_FAILED);
    modules.PublishSbsVrProviders(true);
    for (u32 id : {0x8fu, 0xd4u, 0xedu, 0x8000001au, 0xebu}) {
        check("SBS publication is not a load", modules.Handle(id) == ORBIS_SYSMODULE_NOT_LOADED);
        s32 vr_handle = -1;
        check("SBS family load", modules.Load(id) == 0 && modules.Handle(id, &vr_handle) == 0);
        if (id == 0x8000001a)
            check("guest Camera provider takes precedence", vr_handle == 99);
        modules.PublishSbsVrProviders(true);
        check("SBS repeated load", modules.Load(id) == 0);
        check("SBS first release retains handle", modules.Unload(id) == 0 && modules.Handle(id) == 0);
        check("SBS final release hides handle", modules.Unload(id) == 0 && modules.Handle(id) == ORBIS_SYSMODULE_NOT_LOADED);
        s32 reloaded = -1;
        check("SBS reload keeps provider identity", modules.Load(id) == 0 && modules.Handle(id, &reloaded) == 0 && reloaded == vr_handle);
        check("SBS final cleanup", modules.Unload(id) == 0);
    }
    check("SBS leaves unrelated missing provider unavailable", modules.Load(0xab) == ORBIS_SYSMODULE_LOCK_FAILED);
    std::array<u64, 6> args{0xd7, 0, 0, 0, base, 0};
    auto load = [&] { return LoadInitializedSysmodule(modules, *space, args); };
    check("prepare sentinel", set(base, 123));
    args[0] = 0xab;
    check("missing provider does not fake init",
          load() == u32(ORBIS_SYSMODULE_LOCK_FAILED) && get(base) == 123);
    args[0] = 0xff;
    check("unknown id", load() == u32(ORBIS_SYSMODULE_INVALID_ID));
    args[0] = 0xd7;
    args[3] = 1;
    check("reserved argument", load() == u32(ORBIS_SYSMODULE_INVALID_ID));
    args[3] = 0;
    args[1] = UINT32_MAX;
    check("negative byte count", load() == u32(ORBIS_KERNEL_ERROR_EINVAL));
    args[1] = 1024 * 1024 + 1;
    check("bounded byte count", load() == u32(ORBIS_KERNEL_ERROR_EINVAL));
    args[1] = 4;
    args[2] = 1;
    check("bad input before reference", load() == u32(ORBIS_KERNEL_ERROR_EFAULT));
    args[2] = base;
    args[4] = 1;
    check("bad output before reference", load() == u32(ORBIS_KERNEL_ERROR_EFAULT));
    check("all failures preserve refs and output",
          modules.Handle(0xd7) == ORBIS_SYSMODULE_NOT_LOADED && get(base) == 123);
    args[4] = base;
    check("initialized provider publishes result", load() == 0 && get(base) == 0);
    s32 handle{};
    check("actual provider handle", modules.Handle(0xd7, &handle) == 0 && handle == 31);
    set(base, 456);
    check("repeat load leaves start result alone", load() == 0 && get(base) == 456);
    check("first unload retains ref", modules.Unload(0xd7) == 0 && modules.Handle(0xd7) == 0);
    check("last unload hides provider",
          modules.Unload(0xd7) == 0 && modules.Handle(0xd7) == ORBIS_SYSMODULE_NOT_LOADED);
    args[1] = 0;
    args[2] = UINT64_MAX;
    args[4] = 0;
    check("zero args and optional output", load() == 0);
    check("reload uses stable provider", modules.Handle(0xd7, &handle) == 0 && handle == 31);
    check("Json2 absent before explicit desktop policy",
          modules.Load(0xe7) == ORBIS_SYSMODULE_LOCK_FAILED);
    modules.AllowDesktopJson2Compatibility();
    check("desktop optional Json2 bookkeeping", modules.Load(0xe7) == 0 &&
          modules.Handle(0xe7, &handle) == 0 && handle == 0x10000100);
    check("compatibility does not admit arbitrary missing providers",
          modules.Load(0xab) == ORBIS_SYSMODULE_LOCK_FAILED);
    check("compatibility reference release", modules.Unload(0xe7) == 0 &&
          modules.Handle(0xe7) == ORBIS_SYSMODULE_NOT_LOADED);
    modules.Publish("libSceJson2", 41);
    check("actual provider takes precedence", modules.Load(0xe7) == 0 &&
          modules.Handle(0xe7, &handle) == 0 && handle == 41);
    args = {1, 0, 0, base, base + 4, base + 8};
    for (const auto nid : DiscMapNids) {
        set(base, 5);
        set(base + 4, 6);
        set(base + 8, 7);
        const auto status = DispatchDiscMap(*space, nid, args);
        check("disc map exact registered NID", IsDiscMapNid(nid));
        check("desktop optical capability semantics",
              nid == "fJgP+wqifno"
                  ? status == 0 && get(base) == 0 && get(base + 4) == 0 && get(base + 8) == 0
                  : status == u32(ORBIS_DISC_MAP_ERROR_NO_BITMAP_INFO) && get(base) == 5 &&
                        get(base + 4) == 6 && get(base + 8) == 7);
    }
    for (unsigned i = 3; i < 6; ++i) {
        args = {1, 0, 0, base, base + 4, base + 8};
        args[i] = 1;
        set(base, 5);
        set(base + 4, 6);
        set(base + 8, 7);
        check("all output pins before any write",
              DispatchDiscMap(*space, "fJgP+wqifno", args) == u32(ORBIS_KERNEL_ERROR_EFAULT) &&
                  get(base) == 5 && get(base + 4) == 6 && get(base + 8) == 7);
    }
    check("unknown DiscMap import not admitted", !IsDiscMapNid("unknown"));
    std::printf("GUEST_SYSMODULE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
