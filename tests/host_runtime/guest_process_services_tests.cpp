// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_audio_input.h"
#include "core/host_runtime/guest_process_services.h"
#include "core/host_runtime/guest_backtrace.h"
#include "core/host_runtime/guest_kernel_time.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
int main() {
    AddressSpaceConfig config{}; config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config); if (!made) return 2;
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    CHECK(space->Map({{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    {
        std::array<u8, 64> sentinel{}; sentinel.fill(0xa5);
        auto reset = [&] {
            CHECK(space->WriteData({base}, std::as_bytes(std::span{sentinel})));
        };
        auto read = [&] {
            std::array<u8, 64> bytes{};
            CHECK(space->ReadData({base}, std::as_writable_bytes(std::span{bytes})));
            return bytes;
        };
        const std::array<GuestKernelTimezone, 3> zones{{{}, {480, 0}, {-300, 60}}};
        for (const auto zone : zones) {
            reset();
            CHECK(DispatchKernelTimezone(*space, "kOcnerypnQA", {base}, zone) == 0);
            auto result = read();
            CHECK(std::memcmp(result.data(), &zone, sizeof(zone)) == 0);
            CHECK(std::equal(result.begin() + 8, result.end(), sentinel.begin() + 8));
            for (u64 epoch : std::array<u64, 4>{0, UINT64_MAX, 1709164800ULL, 0x7fffffffffffffffULL}) {
                const u64 expected_local = epoch + u64((s64(zone.offset_minutes) + zone.dst_minutes) * 60);
                for (bool to_utc : {false, true}) {
                    const auto nid = to_utc ? "0NTHN1NKONI" : "-o5uEDpN+oY";
                    const size_t first = to_utc ? 2 : 1;
                    for (u32 optional = 0; optional < 8; ++optional) {
                        reset();
                        std::array<u64, 6> args{};
                        args[0] = to_utc ? expected_local : epoch;
                        if (to_utc) args[1] = UINT64_MAX; // Fixed UTC profile has no DST fold.
                        if (optional & 1) args[first] = base;
                        if (optional & 2) args[first + 1] = base + 16;
                        if (optional & 4) args[first + 2] = base + 40;
                        CHECK(DispatchKernelTimezone(*space, nid, args, zone) == 0);
                        auto want = sentinel;
                        const u64 expected = to_utc ? epoch : expected_local;
                        const GuestKernelTimeInfo info{epoch, zone.offset_minutes * 60, zone.dst_minutes * 60};
                        const s32 dst = zone.dst_minutes * 60;
                        if (optional & 1) std::memcpy(want.data(), &expected, 8);
                        if (optional & 2) std::memcpy(want.data() + 16, &info, 16);
                        if (optional & 4) std::memcpy(want.data() + 40, &dst, 4);
                        CHECK(read() == want); // Includes all canaries around 8/16/4 outputs.
                    }
                    for (size_t bad_output = 0; bad_output < 3; ++bad_output) {
                        for (u64 bad : {base + 0x3ffe, UINT64_MAX}) {
                            reset();
                            std::array<u64, 6> args{};
                            args[0] = epoch;
                            args[first] = base;
                            args[first + 1] = base + 16;
                            args[first + 2] = base + 40;
                            args[first + bad_output] = bad;
                            CHECK(DispatchKernelTimezone(*space, nid, args, zone) == u32(ORBIS_KERNEL_ERROR_EFAULT));
                            CHECK(read() == sentinel); // No partial success before bad output.
                        }
                    }
                }
            }
        }
        CHECK(DispatchKernelTimezone(*space, "kOcnerypnQA", {}) == 0);
        CHECK(DispatchKernelTimezone(*space, "kOcnerypnQA", {base + 0x3ffe}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
        CHECK(DispatchKernelTimezone(*space, "unknown", {}) == u32(ORBIS_KERNEL_ERROR_ENOSYS));
    }
    Core::OrbisKernelModuleInfoEx source{};
    source.id = 7; source.name[0] = 'x'; source.eh_frame_hdr_addr = base + 0x3100;
    source.eh_frame_addr = base + 0x3200; source.eh_frame_size = 0x123;
    source.segment_count = 1; source.segments[0] = {base + 0x2000, 0x1000, 5};
    auto lookup = [&](u64 pc) -> std::optional<Core::OrbisKernelModuleInfoEx> {
        if (pc >= base + 0x2000 && pc < base + 0x3000) return source;
        return std::nullopt;
    };
    auto hide = [](const char* name) { return name[0] == 'x'; };
    using Info = Libraries::Kernel::OrbisModuleInfoForUnwind;
    Info info{}; info.st_size = sizeof(Info); info.name.fill('!');
    auto reset = [&] { CHECK(space->WriteData({base}, std::as_bytes(std::span{&info, 1}))); };
    auto invoke = [&](bool unwind, bool sys, u64 pc, int flags, u64 out) {
        return DispatchModuleInfo(*space, unwind, sys, pc, flags, out, lookup, hide);
    };
    reset();
    for (int flags : {-1, 3}) {
        CHECK(invoke(true, false, base + 0x2000, flags, base) == u32(ORBIS_KERNEL_ERROR_EINVAL));
        Info unchanged{};
        CHECK(space->ReadData({base}, std::as_writable_bytes(std::span{&unchanged, 1})));
        CHECK(std::memcmp(&unchanged, &info, sizeof(info)) == 0);
    }
    CHECK(invoke(true, false, base + 0x2000, 0, 0) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(invoke(true, false, base + 0x2000, 0, base + 0x3ff0) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(invoke(true, false, base + 0x3000, 0, base) == u32(ORBIS_KERNEL_ERROR_ESRCH));
    for (bool sys : {false, true}) {
        reset(); CHECK(invoke(true, sys, base + 0x2000, 1, base) == 0);
        Info result{}; CHECK(space->ReadData({base}, std::as_writable_bytes(std::span{&result, 1})));
        CHECK(result.st_size == sizeof(Info) && result.eh_frame_addr == source.eh_frame_addr);
        CHECK(result.eh_frame_hdr_addr == source.eh_frame_hdr_addr && result.eh_frame_size == 0x123);
        CHECK(result.seg0_addr == base + 0x2000 && result.seg0_size == 0x1000);
        CHECK(result.name[0] == (sys ? 0 : 'x'));
    }
    info.st_size = sizeof(Info) - 1; reset();
    CHECK(invoke(true, false, base + 0x2000, 0, base) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(invoke(false, false, base + 0x2000, 2, base) == 0);
    Core::OrbisKernelModuleInfoEx result{};
    CHECK(space->ReadData({base}, std::as_writable_bytes(std::span{&result, 1})));
    CHECK(result.id == 7 && result.segment_count == 1 && result.segments[0].address == base + 0x2000);
    CHECK(DispatchUnavailableCoredump("8zLSfEfW5AU", 0, 0x4000) == u32(ORBIS_COREDUMP_ERROR_PARAM));
    CHECK(DispatchUnavailableCoredump("8zLSfEfW5AU", 1, 0x20000000) == u32(ORBIS_COREDUMP_ERROR_PARAM));
    for (int i = 0; i < 2; ++i) {
        CHECK(DispatchUnavailableCoredump("8zLSfEfW5AU", base + 0x2000, 0x4000) == u32(ORBIS_COREDUMP_ERROR_THREAD_CREATE));
        CHECK(DispatchUnavailableCoredump("fFkhOgztiCA", 0, 0) == u32(ORBIS_COREDUMP_ERROR_NOT_REGISTERED));
    }
    CHECK(DispatchUnavailableCoredump("Uxqkdta7wEg", 1, 0) == u32(ORBIS_COREDUMP_ERROR_NOT_IN_COREDUMP_HANDLER));
    CHECK(s64(DispatchUnavailableCoredump("Dbbkj6YHWdo", UINT64_MAX, UINT64_MAX)) == ORBIS_COREDUMP_ERROR_NOT_IN_COREDUMP_HANDLER);
    CHECK(DispatchUnavailableCoredump("8zLSfEfW5AU", 1, 0x3fff) == u32(ORBIS_COREDUMP_ERROR_PARAM));
    CHECK(space->Map({{base+0x4000}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    {
        RegisterFile regs{};
        CHECK(CaptureGuestBacktrace(*space, regs).stop == "registers_unavailable");
        regs.validity = RegisterValidity::Gpr;
        regs.Set(Gpr::Rdi, base + 0x200);
        regs.Set(Gpr::Rsp, base + 0x300);
        regs.Set(Gpr::Rbp, base + 0x3ff8);
        const std::array<char, 5> prefix{'t','e','s','t',0};
        CHECK(space->WriteData({base + 0x200}, std::as_bytes(std::span{prefix})));
        const u64 caller = 0x123456789;
        CHECK(space->WriteData({base + 0x300}, std::as_bytes(std::span{&caller, 1})));
        // Frame record deliberately straddles two independently mapped pages.
        std::array<u64, 2> record{base + 0x4100, 0x23456789a};
        CHECK(space->WriteData({base + 0x3ff8}, std::as_bytes(std::span{&record[0], 1})));
        CHECK(space->WriteData({base + 0x4000}, std::as_bytes(std::span{&record[1], 1})));
        std::array<u64, 2> final_record{0, 0x3456789ab};
        CHECK(space->WriteData({base + 0x4100}, std::as_bytes(std::span{final_record})));
        auto trace = CaptureGuestBacktrace(*space, regs);
        CHECK(trace.stop == "complete" && trace.count == 3 && trace.stopped_at == 0);
        CHECK(trace.addresses[0] == caller && trace.addresses[1] == record[1] &&
              trace.addresses[2] == final_record[1]);
        CHECK(std::string_view(trace.prefix.data(), trace.prefix_size) == "test");
        CHECK(trace.prefix_status == "complete");
        final_record[0] = base + 0x3ff8;
        CHECK(space->WriteData({base + 0x4100}, std::as_bytes(std::span{final_record})));
        CHECK(CaptureGuestBacktrace(*space, regs).stop == "nonascending_frame");
        for (u64 bad : {base + 0x301, base + 0x2f8}) {
            regs.Set(Gpr::Rbp, bad);
            CHECK(CaptureGuestBacktrace(*space, regs).stop == "invalid_frame");
        }
        regs.Set(Gpr::Rbp, UINT64_MAX - 7);
        CHECK(CaptureGuestBacktrace(*space, regs).stop == "unreadable_frame");
        regs.Set(Gpr::Rbp, 0);
        regs.Set(Gpr::Rdi, 0);
        trace = CaptureGuestBacktrace(*space, regs);
        CHECK(trace.stop == "complete" && trace.count == 1 && trace.prefix_status == "null");
        regs.Set(Gpr::Rdi, UINT64_MAX);
        CHECK(CaptureGuestBacktrace(*space, regs).prefix_status == "unreadable");
        std::array<char, 1024> long_prefix{}; long_prefix.fill('x');
        CHECK(space->WriteData({base + 0x1000}, std::as_bytes(std::span{long_prefix})));
        regs.Set(Gpr::Rdi, base + 0x1000);
        CHECK(CaptureGuestBacktrace(*space, regs).prefix_status == "truncated");
        regs.Set(Gpr::Rdi, base + 0x7fff);
        const char x = 'x';
        CHECK(space->WriteData({base + 0x7fff}, std::as_bytes(std::span{&x, 1})));
        trace = CaptureGuestBacktrace(*space, regs);
        CHECK(trace.prefix_size == 1 && trace.prefix_status == "unreadable");
        regs.Set(Gpr::Rdi, 0);
        // A long, valid chain must terminate with an explicit limit marker.
        for (u64 offset = 0x1000; offset < 0x2100; offset += 16) {
            record = {base + offset + 16, offset};
            CHECK(space->WriteData({base + offset}, std::as_bytes(std::span{record})));
        }
        regs.Set(Gpr::Rbp, base + 0x1000);
        trace = CaptureGuestBacktrace(*space, regs);
        CHECK(trace.stop == "frame_limit" && trace.count == 256);
        regs.Set(Gpr::Rsp, base + 0x8000);
        CHECK(CaptureGuestBacktrace(*space, regs).stop == "unreadable_return");
    }
    std::array<u8, 64> code{};
    const std::array<u8, 19> sig{0x48,0x8d,0x7c,0x24,0x40,0x6a,0,0x48,0xc7,0xc0,0xa1,1,0,0,0x0f,5,0xf4,0xeb,0xfd};
    std::copy(sig.begin(), sig.end(), code.begin());
    const std::array<u8, 5> call{0x49,0x89,0xca,0x0f,5};
    std::copy(call.begin(), call.end(), code.begin()+32);
    CHECK(space->WriteData({base+0x4000}, std::as_bytes(std::span{code})));
    CHECK(ClassifyGuestSignalReturn(*space, base+0x4000) == 0);
    CHECK(space->Protect({{base+0x4000}, 0x4000}, GuestPermission::Read | GuestPermission::Execute));
    CHECK(ClassifyGuestSignalReturn(*space, base+0x4000) == 1);
    CHECK(ClassifyGuestSignalReturn(*space, base+0x4025) == 2);
    CHECK(ClassifyGuestSignalReturn(*space, base+0x4001) == 0);
    CHECK(ClassifyGuestSignalReturn(*space, base+0x7fff) == 0);
    CHECK(ClassifyGuestSignalReturn(*space, 1) == 0);
    using namespace Libraries::VrTracker;
    OrbisVrTrackerCpuProcessParam cpu{}; cpu.size = sizeof(cpu);
    OrbisVrTrackerNotifyEndOfCpuProcessParam end{}; end.size = sizeof(end);
    CHECK(sceVrTrackerCpuProcess(&cpu) == ORBIS_VR_TRACKER_ERROR_NOT_INIT);
    GuestVrSensor::Instance().SetSbsEnabled(true);
    OrbisVrTrackerInitParam init{}; init.size = sizeof(init);
    init.direct_memory_onion = reinterpret_cast<void*>(0x10000);
    init.direct_memory_garlic = reinterpret_cast<void*>(0x20000);
    init.work_memory = reinterpret_cast<void*>(0x30000);
    init.direct_memory_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    init.direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    init.work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    init.direct_memory_onion_alignment = init.direct_memory_garlic_alignment = init.work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    for (int round = 0; round < 2; ++round) {
        CHECK(sceVrTrackerInit(&init) == 0);
        CHECK(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_HMD, 123) == 0);
        cpu.operation_mode = ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_WHOLE;
        CHECK(sceVrTrackerCpuProcess(&cpu) == 0);
        cpu.operation_mode = ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_HANDLE; cpu.handle = 123;
        CHECK(sceVrTrackerCpuProcess(&cpu) == 0);
        cpu.handle = 124;
        CHECK(sceVrTrackerCpuProcess(&cpu) == ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED);
        CHECK(sceVrTrackerNotifyEndOfCpuProcess(&end) == 0);
        end.size = 0;
        CHECK(sceVrTrackerNotifyEndOfCpuProcess(&end) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        end.size = sizeof(end);
        CHECK(sceVrTrackerCpuProcess(nullptr) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        CHECK(sceVrTrackerTerm() == 0);
        CHECK(sceVrTrackerNotifyEndOfCpuProcess(&end) == ORBIS_VR_TRACKER_ERROR_NOT_INIT);
    }
    GuestVrSensor::Instance().SetSbsEnabled(false);
#if defined(__ANDROID__)
    std::array<u64, 6> mic{1, 1, 0, 512, 48000, 0};
    CHECK(DispatchAudioInput("5NE8Sjc7VC8", mic) == u32(ORBIS_AUDIO_IN_ERROR_NOT_OPENED));
    mic[3] = 0;
    CHECK(DispatchAudioInput("5NE8Sjc7VC8", mic) == u32(ORBIS_AUDIO_IN_ERROR_INVALID_SIZE));
    mic[3] = 512; mic[4] = 44100;
    CHECK(DispatchAudioInput("5NE8Sjc7VC8", mic) == u32(ORBIS_AUDIO_IN_ERROR_INVALID_FREQ));
    mic[4] = 48000; mic[5] = 99;
    CHECK(DispatchAudioInput("5NE8Sjc7VC8", mic) == u32(ORBIS_AUDIO_IN_ERROR_INVALID_PARAM));
    for (auto nid : {"Jh6WbHhnI68", "BohEAQ7DlUE", "LozEOU8+anM"}) {
        mic = {0, UINT64_MAX};
        CHECK(DispatchAudioInput(nid, mic) == u32(ORBIS_AUDIO_IN_ERROR_INVALID_HANDLE));
        mic[0] = 0x30000000;
        CHECK(DispatchAudioInput(nid, mic) == u32(ORBIS_AUDIO_IN_ERROR_NOT_OPENED));
    }
    mic[1] = 0;
    CHECK(DispatchAudioInput("LozEOU8+anM", mic) == u32(ORBIS_AUDIO_IN_ERROR_INVALID_POINTER));
#endif
    std::printf("PROCESS_SERVICES checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
