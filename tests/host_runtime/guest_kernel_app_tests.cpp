// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_kernel_app.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace Libraries::Kernel;

int main() {
    unsigned checks{}, failures{};
    auto check = [&](bool pass) {
        ++checks;
        if (!pass) {
            ++failures;
            std::printf("FAIL %u\n", checks);
        }
    };
    AddressSpaceConfig config{};
    config.reservation_size = 65536;
    auto made = GuestAddressSpace::Create(config);
    if (!made) return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    if (!space->Map({{base}, 65536}, GuestPermission::Read | GuestPermission::Write)) return 3;
    check(GuestGetAppInfo(*space, 0, base, "CUSA12392") == u32(ORBIS_KERNEL_ERROR_EPERM));
    check(GuestGetAppInfo(*space, 0xbad1, 0, "CUSA12392") == 0);
    check(GuestGetAppInfo(*space, 0xbad1, 1, "CUSA12392") == u32(ORBIS_KERNEL_ERROR_EFAULT));
    check(GuestGetAppInfo(*space, 0xbad1, base, "CUSA12392") == 0);
    OrbisKernelAppInfo info{};
    check(bool(space->ReadData({base}, std::as_writable_bytes(std::span{&info, 1}))));
    check(std::string_view(info.cusa_name) == "CUSA12392" && info.has_param_sfo == 1);
    check(info.title_workaround.ids[0] == 0 && info.title_workaround.ids[1] == 0);
    OrbisKernelTitleWorkaround workaround{};
    workaround.ids[0] = 1ULL << 57;
    check(bool(space->WriteData({base + 128}, std::as_bytes(std::span{&workaround, 1}))));
    const auto request = [&](s32 bit) { return GuestTitleWorkaround(*space, base + 128, bit, base + 256); };
    check(request(57) == 0);
    s32 value{};
    check(bool(space->ReadData({base + 256}, std::as_writable_bytes(std::span{&value, 1}))) && value == 1);
    check(request(-1) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    check(request(58) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    check(bool(space->ReadData({base + 256}, std::as_writable_bytes(std::span{&value, 1}))) && value == 1);
    check(request(0) == 0);
    check(bool(space->ReadData({base + 256}, std::as_writable_bytes(std::span{&value, 1}))) && value == 0);
    check(GuestTitleWorkaround(*space, 0, 0, base) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    check(GuestTitleWorkaround(*space, base, 0, 0) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    check(GuestTitleWorkaround(*space, base + 65530, 0, base) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    check(GuestTitleWorkaround(*space, base + 128, 0, base + 65534) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    std::printf("guest_kernel_app_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
