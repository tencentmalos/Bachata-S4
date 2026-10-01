// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include "core/host_runtime/guest_vr_service_dialog.h"
using namespace Core::GuestCpu;
using Core::HostRuntime::GuestVrServiceDialog;
using DialogError = Libraries::CommonDialog::Error;
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 1 << 20;
    auto created = GuestAddressSpace::Create(config);
    if (!created)
        return 2;
    auto space = std::move(created).Value();
    const auto b = space->ReservationBase();
    if (!space->Map({b, 0x4000}, GuestPermission::Read | GuestPermission::Write))
        return 2;
    unsigned checks{}, failures{};
    auto check = [&](bool ok) {
        ++checks;
        failures += !ok;
        if (!ok)
            std::printf("FAIL %u\n", checks);
    };
    auto call = [&](std::string_view nid, u64 arg = 0) {
        return GuestVrServiceDialog::Invoke(*space, nid, {arg});
    };
    for (const auto nid : GuestVrServiceDialog::Nids)
        check(GuestVrServiceDialog::IsNid(nid));
    check(!GuestVrServiceDialog::IsNid("unknown"));
    check(call("RmRtBJpoHlA") == 0);
    check(call("kUavKmsczkY") == 0);
    check(call("hYFXG8FWThI") == u32(DialogError::NOT_SUPPORTED));
    check(call("hYFXG8FWThI") == u32(DialogError::NOT_SUPPORTED));
    check(call("kUavKmsczkY") == 0); // failed initialization must not create a client
    check(call("M4xKWUytNMo") == u32(DialogError::NOT_INITIALIZED));
    check(call("hBH2ABP7IeY") == u32(DialogError::NOT_INITIALIZED));
    std::array<u8, 0x68> param{};
    const auto write = [&] { check(bool(space->WriteData(b, std::as_bytes(std::span{param})))); };
    auto put = [&](size_t offset, auto value) {
        std::memcpy(param.data() + offset, &value, sizeof(value));
    };
    write();
    check(call("60-cjn5Dn0Q") == u32(DialogError::ARG_NULL));
    check(call("60-cjn5Dn0Q", b.value + 0x3ff0) == u32(DialogError::ARG_NULL));
    check(call("60-cjn5Dn0Q", b.value) == u32(DialogError::PARAM_INVALID));
    put(0, u64(0x30));
    put(0x2c, u32(b.value + 0xc0d1a109));
    put(0x30, u64(0x68));
    write();
    check(call("60-cjn5Dn0Q", b.value) == u32(DialogError::NOT_INITIALIZED));
    put(0x38, u32(3));
    write();
    check(call("60-cjn5Dn0Q", b.value) == u32(DialogError::PARAM_INVALID));
    put(0x38, u32(2));
    param[0x67] = 1;
    write();
    check(call("60-cjn5Dn0Q", b.value) == u32(DialogError::PARAM_INVALID));
    param.fill(0);
    put(0, u32(0xdeadbeef));
    write();
    check(call("cYnBkgm8I0c") == u32(DialogError::ARG_NULL));
    check(call("cYnBkgm8I0c", b.value) == u32(DialogError::NOT_INITIALIZED));
    std::array<u8, 0x68> after{};
    check(bool(space->ReadData(b, std::as_writable_bytes(std::span{after}))));
    check(after == param); // no synthetic result or reserved/output mutation
    for (size_t i = 4; i < 0x24; ++i) {
        param[i] = 1;
        write();
        check(call("cYnBkgm8I0c", b.value) == u32(DialogError::PARAM_INVALID));
        param[i] = 0;
    }
    check(call("unknown") == u32(DialogError::NOT_SUPPORTED));
    std::printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
