// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reuse of streamed copies (BufferCache::ObtainBuffer). A small read-only guest buffer bound
// again inside one window is bound at its earlier stream-buffer copy instead of being copied
// again: games bind the same constants (view and frame uniform buffers) in every draw, about
// 80 MB per frame on KH3. A window lasts while nothing could have changed what the copy holds:
//
//  - the same command buffer (scheduler tick): the stream buffer recycles a region once the GPU
//    finished the tick that last wrote it, so a copy may only be bound in the tick it was made;
//  - no command processor event that writes or waits on guest memory or lets the guest and the
//    other queues run (EndWindow): packets other than register writes, draws, dispatches,
//    markers, cache maintenance and events that write nothing; queue yields; new submissions;
//    host commands; flips;
//  - no host write into guest memory and no mapping change (Core::MemoryManager::GuestViewEpoch):
//    write-backs, fence labels, HLE copies, map/unmap/commit/decommit, and the guest writes
//    the HLE reports through InvalidateMemory.
//
// A plain guest CPU write is not observed. A game that rewrites a buffer the GPU has yet to read,
// with no label, wait or new submission in between, races the GPU on the console too.
//
// GPU writes are not this table's concern: ObtainBuffer streams only ranges that are not
// GPU-modified, and checks that before looking here.

#pragma once

#include <array>
#include <atomic>
#include <bit>

#include "common/types.h"
#include "video_core/amdgpu/pm4_opcodes.h"

namespace VideoCore::StreamReuse {

// `upload_diag stream_reuse on|off`, debug.shadps4.stream_reuse=0|1, SHADPS4_STREAM_REUSE.
inline std::atomic<bool> enabled{true};

enum class End : u32 { Packet, Yield, Submission, Commands, Flip, Count };

// Command processor windows; written by the GPU command thread only.
inline std::atomic<u64> window{1};
inline std::array<std::atomic<u64>, static_cast<size_t>(End::Count)> ends{};
// Window ends by packet opcode.
inline std::array<std::atomic<u64>, 256> packet_ends{};

inline void EndWindow(End reason) {
    window.fetch_add(1, std::memory_order_relaxed);
    ends[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

inline void EndWindow(AmdGpu::PM4ItOpcode opcode) {
    EndWindow(End::Packet);
    packet_ends[static_cast<u32>(opcode) & 0xff].fetch_add(1, std::memory_order_relaxed);
}

/// Packets that keep the window open: they neither write nor wait on guest memory. Anything
/// else (labels, WRITE_DATA, DMA, waits, events, constant engine dumps, packets added later)
/// ends it.
constexpr bool KeepsWindow(AmdGpu::PM4ItOpcode opcode) {
    using Op = AmdGpu::PM4ItOpcode;
    switch (opcode) {
    case Op::Nop: // flips are ended separately (End::Flip)
    case Op::SetBase:
    case Op::ClearState:
    case Op::ContextControl:
    case Op::IndexBufferSize:
    case Op::IndexBase:
    case Op::IndexType:
    case Op::NumInstances:
    case Op::DispatchDirect:
    case Op::DispatchIndirect:
    case Op::DrawIndirect:
    case Op::DrawIndexIndirect:
    case Op::DrawIndex2:
    case Op::DrawIndirectMulti:
    case Op::DrawIndexAuto:
    case Op::DrawIndexMultiAuto:
    case Op::DrawIndexOffset2:
    case Op::DrawIndexIndirectMulti:
    case Op::DrawIndexIndirectCountMulti:
    case Op::IndirectBuffer:
    case Op::IndirectBufferConst:
    case Op::SetPredication:
    case Op::PredExec:
    case Op::PreambleCntl:
    case Op::PfpSyncMe:
    // Cache maintenance: the write-backs it may cause go through MemoryManager (GuestViewEpoch).
    case Op::AcquireMem:
    case Op::SurfaceSync:
    // Only the occlusion counter dump writes memory; Liverpool ends the window for that one.
    case Op::EventWrite:
    case Op::LoadShReg:
    case Op::LoadConfigReg:
    case Op::LoadContextReg:
    case Op::SetConfigReg:
    case Op::SetContextReg:
    case Op::SetContextRegIndirect:
    case Op::SetShReg:
    case Op::SetShRegOffset:
    case Op::SetQueueReg:
    case Op::SetUconfigReg:
        return true;
    default:
        return false;
    }
}

/// What a copy depends on; a copy is reused only while all three are unchanged.
struct Window {
    u64 tick{};   ///< scheduler tick the copy was committed in
    u64 window{}; ///< command processor window (StreamReuse::window)
    u64 memory{}; ///< Core::MemoryManager::GuestViewEpoch() before the copy was read
};

enum class Miss : u32 { Empty, Tick, Window, Memory, Count };

/// Direct-mapped table of the latest copy of each (address, size). Buffer is the stream buffer
/// type; GPU command thread only.
template <typename Buffer, size_t Slots = 2048>
class Table {
    static_assert(std::has_single_bit(Slots));

public:
    struct Copy {
        u64 address{};
        u32 size{};
        const Buffer* buffer{};
        u64 offset{};
        Window window{};
    };

    /// The copy of exactly [address, address + size) made in `now`, or null; `miss` receives
    /// why there is none when an older copy of the same range was found (else Miss::Empty).
    const Copy* Find(u64 address, u32 size, const Window& now, Miss* miss = nullptr) const {
        const Copy& copy = slots[Slot(address, size)];
        if (!copy.buffer || copy.address != address || copy.size != size) {
            if (miss) {
                *miss = Miss::Empty;
            }
            return nullptr;
        }
        if (copy.window.tick != now.tick || copy.window.window != now.window ||
            copy.window.memory != now.memory) {
            if (miss) {
                *miss = copy.window.tick != now.tick       ? Miss::Tick
                        : copy.window.window != now.window ? Miss::Window
                                                           : Miss::Memory;
            }
            return nullptr;
        }
        return &copy;
    }

    void Insert(u64 address, u32 size, const Buffer* buffer, u64 offset, const Window& made) {
        slots[Slot(address, size)] = {address, size, buffer, offset, made};
    }

    void Clear() {
        slots.fill({});
    }

private:
    static size_t Slot(u64 address, u32 size) {
        const u64 key = (address >> 2) ^ (u64{size} << 40);
        return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - std::countr_zero(Slots)));
    }

    std::array<Copy, Slots> slots{};
};

} // namespace VideoCore::StreamReuse
