// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"

namespace Core {

/// Mapped guest ranges remembered for a caller that copies from the same mappings over and over
/// (the GPU thread's streamed buffers): a copy inside one is a range compare and a memcpy instead
/// of a mapping lookup. See MemoryManager::CopySparseMemory(..., GuestReadCache&). Entries are
/// dropped whenever a writer has held the VM lock since they were filled. Not thread safe: one
/// cache per thread.
struct GuestReadCache {
    struct Range {
        VAddr begin{};
        VAddr end{};
    };
    std::array<Range, 8> ranges{};
    u64 epoch{~u64{0}};
    u32 next{};
    u64 hits{};
    u64 misses{};
};

} // namespace Core
