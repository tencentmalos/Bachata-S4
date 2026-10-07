// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <span>

#include "common/types.h"

/// Notifications the GPU replay recorder needs from outside the command processor
/// (docs/specs/gpu-replay-20261004.md). Each costs one relaxed load while no capture runs.
namespace VideoCore::Replay {

namespace Detail {
inline std::atomic<bool> capture_hooks{};
inline std::atomic<bool> replaying{};
inline std::atomic<bool> draw_hashes{};
} // namespace Detail

/// GPU replay diagnostics: a draw or dispatch was recorded, with its program hashes. Active only
/// while a replay hashes every draw of one event.
void AfterDrawSlow(const char* kind, u64 hash0, u64 hash1);
inline void AfterDraw(const char* kind, u64 hash0, u64 hash1) {
    if (Detail::draw_hashes.load(std::memory_order_relaxed)) [[unlikely]] {
        AfterDrawSlow(kind, hash0, hash1);
    }
}

/// True while a capture records events.
inline bool CaptureHooksActive() {
    return Detail::capture_hooks.load(std::memory_order_relaxed);
}

/// True in a process that replays a trace.
inline bool Replaying() {
    return Detail::replaying.load(std::memory_order_relaxed);
}
inline void SetReplaying(bool replaying) {
    Detail::replaying.store(replaying, std::memory_order_relaxed);
}

/// Work whose effect on guest memory or on the caches depends on timing must happen in command
/// order while a trace is captured or replayed: guest memory then changes only at the points the
/// trace records.
inline bool DeterministicGpu() {
    return CaptureHooksActive() || Replaying();
}

/// A guest stack (a thread created on caller memory, or a fiber about to run): the recorder
/// must never write-protect it, since Windows writes the exception record onto the stack of the
/// faulting thread. Called before any code runs on the stack.
void NoteGuestStackSlow(VAddr base, u64 size);
inline void NoteGuestStack(VAddr base, u64 size) {
    if (CaptureHooksActive()) [[unlikely]] {
        NoteGuestStackSlow(base, size);
    }
}

/// A guest mapping changed (map, unmap, protect, pool commit). Called by the memory manager
/// after the change, under its writer lock.
void NoteMappingChangeSlow(VAddr base, u64 size, bool protect_only);
inline void NoteMappingChange(VAddr base, u64 size, bool protect_only) {
    if (CaptureHooksActive()) [[unlikely]] {
        NoteMappingChangeSlow(base, size, protect_only);
    }
}

/// A graphics submission whose command buffers are host copies rather than guest memory (the
/// Android GNM HLE copies them before submitting): the trace carries their contents, since a
/// replay cannot read them at the recorded address.
void NoteSubmitContentsSlow(u64 submission, std::span<const u32> dcb, std::span<const u32> ccb);
inline void NoteSubmitContents(u64 submission, std::span<const u32> dcb, std::span<const u32> ccb) {
    if (CaptureHooksActive()) [[unlikely]] {
        NoteSubmitContentsSlow(submission, dcb, ccb);
    }
}

/// sceVideoOutSubmitEopFlip registered a flip for the next patched flip packet. Guest thread,
/// before the command buffer holding that packet is submitted.
void NoteEopFlipArmedSlow(s32 handle, s32 index, s64 flip_arg);
inline void NoteEopFlipArmed(s32 handle, s32 index, s64 flip_arg) {
    if (CaptureHooksActive()) [[unlikely]] {
        NoteEopFlipArmedSlow(handle, index, flip_arg);
    }
}

} // namespace VideoCore::Replay
