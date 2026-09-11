// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Test-only deterministic handshake for SessionCore lifecycle tests.
//
// Standard-library only, so it compiles into both the host unit test and (under
// SESSION_TEST_HOOKS) SessionCore itself. When a checkpoint is NOT armed, Wait()
// returns immediately -- production builds never define SESSION_TEST_HOOKS, so
// the gate is compiled out entirely and costs nothing.
//
// The point is to force the exact interleavings the assessment named ("Stop
// acquired access, owner about to destroy", "context not yet published", ...)
// without a single sleep: the test arms a checkpoint, the core thread blocks
// there until the test releases it, and the test meanwhile drives the other
// thread to the precise racing position.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Core::HostRuntime {

enum class Checkpoint : std::uint32_t {
    // Owner has minted the runtime-less session and is about to publish the
    // runtime (context not yet visible to a stopper).
    BeforePublish = 0,
    // Owner's Run has returned and it is about to enter the teardown drain /
    // Destroy. A stopper parked here is the "owner about to destroy" window.
    OwnerAfterRunBeforeDestroy,
    // A stopper has taken the control-lease and copied the runtime shared_ptr,
    // and is about to call RequestCancel. Parking here holds a lease open.
    StopAfterLeaseBeforeInterrupt,
    kCount,
};

// One gate instance per SessionCore. Thread-safe; unarmed checkpoints are
// pass-through. Arm is one-shot per checkpoint per generation: a released
// checkpoint stays released until re-armed, so a checkpoint hit twice in one
// test blocks only where the test armed it.
class SessionTestGate {
public:
    // Arm a checkpoint for one generation. The next thread to Wait() at this
    // (checkpoint, generation) blocks until Release() or a generation change.
    void Arm(Checkpoint cp, std::uint64_t generation) {
        std::lock_guard lock{mutex_};
        auto& slot = slots_[Index(cp)];
        slot.armed = true;
        slot.released = false;
        slot.generation = generation;
        slot.arrived = false;
        cv_.notify_all();
    }

    // Called by SessionCore. Blocks iff this checkpoint is armed for `generation`
    // and not yet released. Pass-through otherwise.
    void Wait(Checkpoint cp, std::uint64_t generation) {
        std::unique_lock lock{mutex_};
        auto& slot = slots_[Index(cp)];
        if (!slot.armed || slot.generation != generation)
            return;
        slot.arrived = true;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return slot.released || !slot.armed || slot.generation != generation;
        });
    }

    // Test side: block until a core thread has reached the checkpoint, so the
    // test knows the racing thread is parked before it drives the other one.
    void WaitUntilArrived(Checkpoint cp, std::uint64_t generation) {
        std::unique_lock lock{mutex_};
        auto& slot = slots_[Index(cp)];
        cv_.wait(lock, [&] {
            return (slot.arrived && slot.generation == generation) ||
                   !slot.armed || slot.generation != generation;
        });
    }

    // Release a parked thread.
    void Release(Checkpoint cp, std::uint64_t generation) {
        std::lock_guard lock{mutex_};
        auto& slot = slots_[Index(cp)];
        if (slot.generation == generation) {
            slot.released = true;
            slot.armed = false;
        }
        cv_.notify_all();
    }

    // True once a core thread has reached the checkpoint (non-blocking peek).
    [[nodiscard]] bool Arrived(Checkpoint cp, std::uint64_t generation) const {
        std::lock_guard lock{mutex_};
        const auto& slot = slots_[Index(cp)];
        return slot.arrived && slot.generation == generation;
    }

private:
    static constexpr std::size_t Index(Checkpoint cp) {
        return static_cast<std::size_t>(cp);
    }

    struct Slot final {
        bool armed{false};
        bool released{false};
        bool arrived{false};
        std::uint64_t generation{0};
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Slot slots_[static_cast<std::size_t>(Checkpoint::kCount)]{};
};

}  // namespace Core::HostRuntime
