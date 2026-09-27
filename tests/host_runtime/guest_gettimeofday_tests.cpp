// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include "core/host_runtime/guest_gettimeofday.h"

using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace Libraries::Kernel;
namespace {
unsigned checks{}, failures{};
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
// Pre-change implementation, retained only for a same-binary microbenchmark.
[[gnu::noinline]] int Before(GuestAddressSpace& space, const GuestClock& clock, u64 tv) {
    if (!space.ValidateRange({{tv}, sizeof(OrbisKernelTimeval)}, GuestPermission::Write))
        return POSIX_EFAULT;
    OrbisKernelTimespec ts{};
    if (const int error = clock.Read(ORBIS_CLOCK_REALTIME, ts, false)) return error;
    const OrbisKernelTimeval value{ts.tv_sec, ts.tv_nsec / 1000};
    return space.WriteData({tv}, std::as_bytes(std::span{&value, 1})) ? 0 : POSIX_EFAULT;
}
[[gnu::noinline]] int After(GuestAddressSpace& space, const GuestClock& clock, u64 tv) {
    return GuestGettimeofday(space, clock, tv, 0);
}
u64 CpuNs() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) std::abort();
    return u64(t.tv_sec) * 1000000000 + t.tv_nsec;
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    auto made = GuestAddressSpace::Create({.reservation_size = 16ULL << 20});
    if (!made) return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value, page = HostPageSize();
    const auto rw = GuestPermission::Read | GuestPermission::Write;
    Check(bool(space->Map({{base}, page}, rw)), "map output");
    Check(bool(space->Map({{base + page}, page}, rw)), "separate adjacent mapping");
    Check(bool(space->Map({{base + page * 3}, page}, GuestPermission::Read)), "read-only mapping");
    Check(bool(space->Map({{base + page * 4}, page}, GuestPermission::Write)), "write-only mapping");
    GuestClock clock;
    const auto set = [&](u64 at, const auto& v) {
        Check(bool(space->WriteData({at}, std::as_bytes(std::span{&v, 1}))), "set sentinel");
    };
    const auto get = [&]<typename T>(u64 at) {
        T v{};
        Check(bool(space->ReadData({at}, std::as_writable_bytes(std::span{&v, 1}))), "read output");
        return v;
    };
    const auto call = [&](u64 tv, u64 tz = 0) { return GuestGettimeofday(*space, clock, tv, tz); };
    const OrbisKernelTimeval marker{-7, -9};
    for (const u64 at : {base, base + 1, base + page - sizeof(marker)}) {
        OrbisKernelTimespec before{}, after{};
        Check(clock.Read(ORBIS_CLOCK_REALTIME, before, false) == 0, "native clock before");
        Check(call(at) == 0, "timeval valid/alignment/end");
        Check(clock.Read(ORBIS_CLOCK_REALTIME, after, false) == 0, "native clock after");
        const auto value = get.operator()<OrbisKernelTimeval>(at);
        const auto us = value.tv_sec * 1000000 + value.tv_usec;
        Check(value.tv_usec >= 0 && value.tv_usec < 1000000, "microsecond normalization");
        Check(us >= before.tv_sec * 1000000 + before.tv_nsec / 1000 &&
              us <= after.tv_sec * 1000000 + after.tv_nsec / 1000, "live realtime bracket");
    }
    Check(call(0, 0) == 0, "both optional outputs null");
    set(base + 64, OrbisKernelTimezone{-1, -1});
    Check(call(0, base + 64) == 0, "timezone only");
    const auto tz = get.operator()<OrbisKernelTimezone>(base + 64);
    Check(tz.tz_minuteswest == 0 && tz.tz_dsttime == 0, "session UTC");
    Check(call(base, base + 64) == 0, "two outputs");
    set(base, marker);
    for (const auto invalid : {u64{1}, UINT64_MAX - 3, base + 2 * page,
                               base + 3 * page, base + page - 8}) {
        Check(call(invalid) == POSIX_EFAULT, "invalid timeval returns EFAULT");
    }
    Check(call(base, base + 2 * page) == POSIX_EFAULT, "invalid timezone refuses timeval");
    const auto unchanged = get.operator()<OrbisKernelTimeval>(base);
    Check(std::memcmp(&unchanged, &marker, sizeof(marker)) == 0, "no write on invalid optional output");
    Check(call(base + 4 * page) == 0, "write permission does not require read");
    struct Observer final : MemoryObserver {
        unsigned writes{};
        GuestRange last{};
        std::string_view Name() const override { return "clock-test"; }
        void OnGuestWrite(GuestRange range) override { ++writes; last = range; }
    } observer;
    space->AddObserver(&observer);
    Check(call(base) == 0 && observer.writes == 1 && observer.last.base.value <= base &&
              observer.last.End() >= base + sizeof(OrbisKernelTimeval),
          "timeval retains guest write notification");
    space->RemoveObserver(&observer);
    Check(space->Counts().live_pins == 0, "no leaked pins");
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        constexpr unsigned iterations = 200000;
        for (unsigned round = 0; round < 6; ++round) {
            for (unsigned order = 0; order < 2; ++order) {
                const bool optimized = (round + order) % 2;
                const auto fn = optimized ? After : Before;
                for (unsigned i = 0; i < 1000; ++i) if (fn(*space, clock, base)) return 2;
                const auto start = CpuNs();
                for (unsigned i = 0; i < iterations; ++i) if (fn(*space, clock, base)) return 2;
                const auto ns = CpuNs() - start;
                std::printf("BENCH variant=%s round=%u calls=%u cpu_ns_per_call=%.2f\n",
                            optimized ? "after" : "before", round, iterations, double(ns) / iterations);
            }
        }
    }
    std::printf("GETTIMEOFDAY_TESTS checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
