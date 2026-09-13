// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_graphics_hle.h"
using namespace Core::GuestCpu;
using Core::HostRuntime::AcquireGraphicsAdmission;
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made) return 2;
    auto space = std::move(made).Value();
    const auto later = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto admitted = AcquireGraphicsAdmission(*space, {}, later, {});
    CHECK(admitted && admitted.Value().IsValid());
    CHECK(!space->Quiesce(0));
    admitted.Value() = ExecutionLease{};
    auto token = space->Quiesce(0);
    CHECK(token);
    auto busy = AcquireGraphicsAdmission(*space, {}, later, {});
    CHECK(!busy && busy.Category() == ErrorCategory::Busy);
    unsigned waits{};
    auto expired = AcquireGraphicsAdmission(*space, {}, std::chrono::steady_clock::now(), [&] { ++waits; });
    CHECK(!expired && expired.Category() == ErrorCategory::Busy && waits == 0);
    std::stop_source cancel;
    auto stopped = AcquireGraphicsAdmission(*space, cancel.get_token(), later, [&] {
        ++waits;
        cancel.request_stop();
    });
    CHECK(stopped && !stopped.Value().IsValid() && waits == 1);
    CHECK(!space->AcquireExecutionLease()); // cancellation did not break the VM barrier
    auto already = AcquireGraphicsAdmission(*space, cancel.get_token(), later, [&] { ++waits; });
    CHECK(already && !already.Value().IsValid() && waits == 1);
    token.Value() = QuiescenceToken{};
    auto recovered = AcquireGraphicsAdmission(*space, {}, later, {});
    CHECK(recovered && recovered.Value().IsValid());
    std::printf("GRAPHICS_ADMISSION checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
