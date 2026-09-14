// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_hub_registry.h"

#include <chrono>

namespace Core::Diagnostics {

namespace {
std::uint64_t MonotonicNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

DiagnosticsHub& DiagnosticsHub::Instance() {
    static DiagnosticsHub hub;
    return hub;
}

std::shared_ptr<DiagnosticsPublisher> DiagnosticsHub::Register(std::uint64_t generation,
                                                              std::uint64_t pid) {
    if (generation == 0) {
        return nullptr;
    }
    auto publisher = std::make_shared<DiagnosticsPublisher>();
    publisher->SetIdentity(generation, pid);
    {
        std::lock_guard lock(mtx_);
        active_ = publisher;
        active_generation_ = generation;
    }
    return publisher;
}

bool DiagnosticsHub::Revoke(std::uint64_t generation) {
    std::lock_guard lock(mtx_);
    // Only the generation that currently owns the slot may clear it; a stale
    // older teardown arriving after a newer Register must not wipe the live one.
    if (active_generation_ != generation) {
        return false;
    }
    active_.reset();
    active_generation_ = 0;
    return true;
}

bool DiagnosticsHub::QuerySnapshot(DiagnosticsSnapshot& out, std::uint64_t now_ns) const {
    // Copy the shared_ptr under the hub lock, then release the lock before the
    // (lock-free) CopyInto. The reader's shared_ptr keeps the publisher alive even
    // if the owning generation revokes concurrently.
    std::shared_ptr<DiagnosticsPublisher> publisher;
    {
        std::lock_guard lock(mtx_);
        publisher = active_;
    }
    if (!publisher) {
        out = DiagnosticsSnapshot{};
        out.snapshot_ns = now_ns;
        return false;
    }
    publisher->CopyInto(out, now_ns);
    return out.has_session;
}

std::uint64_t DiagnosticsHub::ActiveGeneration() const {
    std::lock_guard lock(mtx_);
    return active_generation_;
}

void DiagnosticsHub::Advance(AdvanceSignal signal, std::uint64_t delta) {
    std::shared_ptr<DiagnosticsPublisher> publisher;
    {
        std::lock_guard lock(mtx_);
        publisher = active_;
    }
    if (publisher) {
        publisher->Advance(signal, MonotonicNs(), delta);
    }
}

void DiagnosticsHub::PublishCount(AdvanceSignal signal, std::uint64_t absolute) {
    std::shared_ptr<DiagnosticsPublisher> publisher;
    {
        std::lock_guard lock(mtx_);
        publisher = active_;
    }
    if (publisher) {
        publisher->PublishCount(signal, absolute, MonotonicNs());
    }
}

}  // namespace Core::Diagnostics
