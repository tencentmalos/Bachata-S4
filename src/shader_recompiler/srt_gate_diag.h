// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace Shader::SrtGateDiag {

/// Measures whether gating SRT walks on (permutation, user data, written pages) would pay off:
/// every walk is still performed, and the diagnostic checks what a gate would have done. A walk
/// "could skip" when the same permutation saw the same user data before and none of the guest
/// pages that walk read were written since (writes noted by the GPU replay player). A skip whose
/// real output differs is counted as wrong: the read set or the write notes are incomplete.
/// Diagnostic only (`srt_gate on|off|status|reset`); off by default.
inline std::atomic<bool> enabled{false};

struct Read {
    u64 address;
    u32 size;
};

/// Command processor thread. `reads` are the guest ranges the walk read.
void NoteWalk(const void* info, std::span<const u32> user_data, std::span<const u32> output,
              std::span<const Read> reads);
/// A guest page (4 KiB) was written (GPU replay: recorded CPU writes).
void NoteWrite(u64 page_address);
/// A flip: frame-level counters.
void NoteFlip();
void Reset();
std::string Status();

} // namespace Shader::SrtGateDiag
