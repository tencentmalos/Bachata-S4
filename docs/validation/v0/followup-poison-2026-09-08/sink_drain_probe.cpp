// R2 verification: does ClearCodeInvalidationSink wait for in-flight callbacks?
//
// The review's probe releases its callback only AFTER Clear returns, so against a correct
// implementation it deadlocks by construction. This one releases the callback from a third thread
// on a timer, so it can observe the ordering either way:
//
//   broken  -> Clear returns while the callback is still running (completed == false)
//   correct -> Clear cannot return until the callback finished  (completed == true)

#include "core/guest_cpu/api/address_space.h"
#include <atomic>
#include <thread>
#include <array>
#include <chrono>
#include <cstdio>
using namespace Core::GuestCpu;

struct WaitingSink final : CodeInvalidationSink {
    std::atomic<bool> entered{false}, release{false}, completed{false};
    std::string_view Name() const override { return "waiting"; }
    Status DiscardTranslations(GuestRange, InvalidationReason) override {
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
        completed.store(true);
        return Ok();
    }
};

int main() {
    auto r = GuestAddressSpace::Create({});
    if (!r) return 2;
    auto space = std::move(r).Value();
    GuestRange range{space->ReservationBase(), HostPageSize()};
    if (!space->Map(range, GuestPermission::Read | GuestPermission::Write)) return 3;

    WaitingSink first, second;
    if (!space->SetCodeInvalidationSink(&first)) return 4;
    auto q = space->Quiesce(1000);
    if (!q) return 5;

    std::array<std::byte, 1> code{std::byte{1}};
    bool publication_ok = false;
    std::thread writer([&] {
        publication_ok = bool(space->PublishCode(q.Value(), {range.base, 1}, code));
    });
    while (!first.entered.load()) std::this_thread::yield();

    // Release the callback shortly after Clear starts waiting. A correct Clear blocks until then.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        first.release.store(true);
    });

    const auto started = std::chrono::steady_clock::now();
    space->ClearCodeInvalidationSink(&first);
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    const bool callback_done_before_clear_returned = first.completed.load();
    auto registered = space->SetCodeInvalidationSink(&second);

    releaser.join();
    writer.join();

    printf("clear_waited_ms=%lld\n", (long long)waited_ms);
    printf("callback_completed_before_clear_returned=%d\n", int(callback_done_before_clear_returned));
    printf("replacement_registered_after_drain=%d\n", int(bool(registered)));
    printf("stale_publication_reported_success=%d\n", int(publication_ok));

    space->ClearCodeInvalidationSink(&second);

    const bool drained = callback_done_before_clear_returned;
    printf("\nR2_drain_enforced=%d\n", int(drained));
    printf("R2_stale_callback_committed=%d\n", int(publication_ok));
    return 0;
}
