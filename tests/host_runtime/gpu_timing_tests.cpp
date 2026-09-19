// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/gpu_timing.h"
#include <cstdio>
#include <limits>
using namespace Common::Profiler;
int main() {
    unsigned checks{}, failures{};
#define TEST(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while(0)
    TEST(GpuElapsedNs(100, 300, 64, .5) == 100.0);
    TEST(GpuElapsedNs(250, 5, 8, 2.0) == 22.0);
    TEST(GpuElapsedNs(UINT64_MAX - 5, 3, 64, 1.0) == 9.0);
    TEST(GpuElapsedNs(0x10fa, 0x2005, 8, 2.0) == 22.0);
    TEST(GpuElapsedNs(10, 10, 64, 1.0) == 0.0);
    TEST(!GpuElapsedNs(100, 99, 64, 1.0));
    TEST(!GpuElapsedNs(0, 128, 8, 1.0));
    TEST(!GpuElapsedNs(0, 1, 0, 1.0));
    TEST(!GpuElapsedNs(0, 1, 65, 1.0));
    TEST(!GpuElapsedNs(0, 1, 64, 0));
    TEST(!GpuElapsedNs(0, 1, 64, -1));
    TEST(!GpuElapsedNs(0, 1, 64, std::numeric_limits<double>::quiet_NaN()));
    TEST(!GpuElapsedNs(0, 1, 64, std::numeric_limits<double>::infinity()));
    TEST(!GpuElapsedNs(0, 10, 64, std::numeric_limits<double>::max()));
    TEST(!GpuElapsedNs(0, uint64_t(INT64_MAX), 64, 1.0));
    TEST(!GpuElapsedNs(0, 2, 64, double(INT64_MAX)));
    TEST(!GpuTimestampWritten(0));
    TEST(!GpuTimestampWritten(UINT64_MAX));
    TEST(GpuTimestampWritten(1));
    // A begin preceding the calibration anchor must never reach the SDK's
    // unsigned clock conversion (spruntime's stale-epoch readback case).
    TEST(!GpuElapsedNs(300, 100, 48, 52.083332));
    TEST(GpuTimingControl({"bad"}) == "error=invalid_arguments\n");
    TEST(GpuTimingControl({"start", "extra"}) == "error=invalid_arguments\n");
    auto generation = Generation();
    GpuTimingControl({"start"}); TEST(Generation() != generation);
    generation = Generation(); GpuTimingControl({"start"}); TEST(Generation() == generation);
    GpuTimingControl({"stop"}); TEST(Generation() != generation);
    auto state = std::make_shared<GpuTimingState>(); state->snapshot.generation = 999;
    RegisterGpuTimingState(state); TEST(GpuTimingControl({}).find("generation: 999") != std::string::npos);
    state.reset(); TEST(GpuTimingControl({}).find("session: unavailable") != std::string::npos);
    printf("GPU_TIMING_TESTS %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
