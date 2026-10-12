// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Renderer seam for the Orbis MemoryManager.
//
// core/memory.cpp used to #include the full Vulkan rasterizer header only to call
// three GPU dirty-tracking methods (MapMemory / UnmapMemory / InvalidateMemory).
// That hard include is the one dependency that stopped the loader-memory-kernel
// closure from linking without video_core (HN1.1 dependency-closure audit).
//
// This interface is that seam: MemoryManager holds a RasterizerHooks*, the Vulkan
// rasterizer implements it on desktop, and a headless host supplies a null
// implementation. Behavior on desktop is unchanged -- the same three calls reach
// the same rasterizer, now through a vtable instead of a concrete-type call.

#pragma once

#include "common/types.h"

namespace Core {

// GPU-side hooks the Orbis MemoryManager notifies on guest map/unmap/invalidate.
// The desktop Vulkan rasterizer implements this; a headless profile uses a no-op.
class RasterizerHooks {
public:
    virtual ~RasterizerHooks() = default;

    // Guest memory became mapped/unmapped: keep GPU tracking in sync.
    virtual void MapMemory(VAddr addr, u64 size) = 0;
    virtual void UnmapMemory(VAddr addr, u64 size) = 0;

    // Pooled memory was committed: like MapMemory, but the contents are undefined until the
    // CPU writes them (the commit may map any free physical pages).
    virtual void CommitPoolMemory(VAddr addr, u64 size) {
        MapMemory(addr, size);
    }

    // Pooled memory is being decommitted: like UnmapMemory, but the renderer may keep GPU
    // contents for a later commit of the same range (see CommitPoolMemory).
    virtual void DecommitPoolMemory(VAddr addr, u64 size) {
        UnmapMemory(addr, size);
    }

    // Guest memory at [addr, addr+size) was written on the CPU side. Returns true
    // if the rasterizer handled (invalidated) tracked data in the range.
    virtual bool InvalidateMemory(VAddr addr, u64 size) = 0;
};

}  // namespace Core
