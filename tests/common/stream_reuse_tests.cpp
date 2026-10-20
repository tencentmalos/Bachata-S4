// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Reuse of streamed copies (video_core/buffer_cache/stream_reuse.h) against a stand-in for
// BufferCache::ObtainBuffer: a guest memory, a stream ring whose regions are recycled once the
// tick that wrote them is done, and the window rules. Every binding must see guest memory as it
// is at the binding. Guest writes reach the GPU command thread only through the events the
// window rules end on (a packet that writes or waits on memory, a new submission, a host write
// or mapping change); a plain unsynchronized CPU write is outside the contract and not tested.
// Header-only, no Vulkan: builds on the desktop and with the NDK.
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "video_core/buffer_cache/stream_reuse.h"

using namespace VideoCore;

static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
        }                                                                                          \
    } while (0)

namespace {

constexpr u64 MemorySize = 256 * 1024;
constexpr u64 RingSize = 64 * 1024;

struct Ring {}; // stands in for StreamBuffer

/// The model: guest memory, the stream ring, the window state and the table.
struct Model {
    std::vector<u8> guest = std::vector<u8>(MemorySize);
    std::vector<u8> ring = std::vector<u8>(RingSize);
    std::vector<u64> ring_tick = std::vector<u64>(RingSize); // tick that last wrote each byte
    u64 ring_offset = 0;
    u64 tick = 1;
    u64 memory_epoch = 1; // MemoryManager::GuestViewEpoch
    Ring stream;
    StreamReuse::Table<Ring, 64> table; // small: collisions happen
    bool reuse = true;
    u64 copies = 0, reuses = 0;

    // The command buffer of `tick` is submitted; the GPU finishes ticks in order, and a ring
    // region is rewritten only after its tick finished (StreamBuffer watches).
    void Submit() {
        ++tick;
    }

    /// ObtainBuffer's stream path: returns the ring offset bound for [address, address + size).
    u64 Bind(u64 address, u32 size) {
        const StreamReuse::Window now{tick, StreamReuse::window.load(), memory_epoch};
        if (reuse) {
            if (const auto* copy = table.Find(address, size, now)) {
                ++reuses;
                // The region must still hold this tick's copy (not recycled).
                for (u32 i = 0; i < size; ++i) {
                    if (ring_tick[copy->offset + i] != tick) {
                        ++failures;
                        std::fprintf(stderr, "FAIL reused region recycled\n");
                        break;
                    }
                }
                return copy->offset;
            }
        }
        ++copies;
        if (ring_offset + size > RingSize) {
            // Wrapping into this tick's data waits for it, which submits it first.
            Submit();
            ring_offset = 0;
        }
        const u64 offset = ring_offset;
        std::memcpy(ring.data() + offset, guest.data() + address, size);
        for (u32 i = 0; i < size; ++i) {
            ring_tick[offset + i] = tick;
        }
        ring_offset += (size + 63) & ~u64{63};
        if (reuse) {
            table.Insert(address, size, &stream, offset, {tick, now.window, now.memory});
        }
        return offset;
    }

    /// Bind and check that the GPU reads what guest memory holds now.
    void BindAndCheck(u64 address, u32 size) {
        const u64 offset = Bind(address, size);
        CHECK(std::memcmp(ring.data() + offset, guest.data() + address, size) == 0);
    }

    // Guest writes, each paired with the event that makes it visible to the GPU command thread.
    // A packet, as Liverpool handles it before executing it.
    static void Packet(AmdGpu::PM4ItOpcode opcode) {
        if (!StreamReuse::KeepsWindow(opcode)) {
            StreamReuse::EndWindow(opcode);
        }
    }
    // The guest writes, then the command processor reaches a packet that waits on or writes
    // guest memory (a wait on the CPU's flag, or a label that tells the CPU the GPU is done).
    void GuestWriteThenLabel(u64 address, u32 size, u8 value, bool wait = true) {
        std::memset(guest.data() + address, value, size);
        Packet(AmdGpu::PM4ItOpcode::SetShReg);
        Packet(wait ? AmdGpu::PM4ItOpcode::WaitRegMem : AmdGpu::PM4ItOpcode::WriteData);
    }
    void GuestWriteThenSubmission(u64 address, u32 size, u8 value) {
        std::memset(guest.data() + address, value, size);
        StreamReuse::EndWindow(StreamReuse::End::Submission);
    }
    void HostWrite(u64 address, u32 size, u8 value) { // write-back, label, HLE copy, remap
        std::memset(guest.data() + address, value, size);
        ++memory_epoch;
    }
};

void TestSameWindowReuses() {
    Model m;
    m.GuestWriteThenSubmission(0x100, 256, 1);
    m.BindAndCheck(0x100, 256);
    m.BindAndCheck(0x100, 256);
    m.BindAndCheck(0x100, 256);
    CHECK(m.copies == 1);
    CHECK(m.reuses == 2);
}

void TestDifferentRangeCopies() {
    Model m;
    m.BindAndCheck(0x100, 256);
    m.BindAndCheck(0x100, 128); // same address, other size
    m.BindAndCheck(0x140, 256);
    CHECK(m.copies == 3);
    CHECK(m.reuses == 0);
}

void TestTickEndsWindow() {
    Model m;
    m.BindAndCheck(0x200, 512);
    m.Submit();
    m.BindAndCheck(0x200, 512);
    CHECK(m.copies == 2);
    StreamReuse::Miss miss{};
    StreamReuse::Table<Ring, 64> table;
    Ring r;
    table.Insert(0x200, 512, &r, 0, {1, 5, 7});
    CHECK(table.Find(0x200, 512, {2, 5, 7}, &miss) == nullptr && miss == StreamReuse::Miss::Tick);
    CHECK(table.Find(0x200, 512, {1, 6, 7}, &miss) == nullptr && miss == StreamReuse::Miss::Window);
    CHECK(table.Find(0x200, 512, {1, 5, 8}, &miss) == nullptr && miss == StreamReuse::Miss::Memory);
    CHECK(table.Find(0x200, 256, {1, 5, 7}, &miss) == nullptr && miss == StreamReuse::Miss::Empty);
    CHECK(table.Find(0x200, 512, {1, 5, 7}, &miss) != nullptr);
}

void TestGuestWriteAfterLabel() {
    Model m;
    m.BindAndCheck(0x300, 64);
    m.GuestWriteThenLabel(0x300, 64, 0xab);
    m.BindAndCheck(0x300, 64); // must see 0xab
    CHECK(m.copies == 2);
}

void TestHostWrite() {
    Model m;
    m.BindAndCheck(0x400, 64);
    m.HostWrite(0x420, 4, 0x5a); // inside the range
    m.BindAndCheck(0x400, 64);
    m.HostWrite(0x8000, 4, 0x11); // elsewhere: epochs are global, the copy is redone anyway
    m.BindAndCheck(0x400, 64);
    CHECK(m.copies == 3);
}

void TestRingWrapSubmits() {
    Model m;
    // Fill the ring: the wrap submits, so copies made before it are no longer reused.
    m.BindAndCheck(0x0, 4096);
    const u64 tick = m.tick;
    for (u64 i = 1; i <= RingSize / 4096; ++i) { // the last one wraps
        m.BindAndCheck(i * 4096, 4096);
    }
    CHECK(m.tick == tick + 1);
    m.BindAndCheck(0x0, 4096);
    CHECK(m.copies == RingSize / 4096 + 2);
}

void TestDisabled() {
    Model m;
    m.reuse = false;
    m.BindAndCheck(0x100, 256);
    m.BindAndCheck(0x100, 256);
    CHECK(m.copies == 2 && m.reuses == 0);
}

void TestPackets() {
    using Op = AmdGpu::PM4ItOpcode;
    for (const Op op : {Op::SetShReg, Op::SetContextReg, Op::SetUconfigReg, Op::DrawIndex2,
                        Op::DrawIndexAuto, Op::DrawIndexOffset2, Op::DispatchDirect,
                        Op::DispatchIndirect, Op::IndexType, Op::NumInstances, Op::Nop,
                        Op::IndirectBuffer, Op::AcquireMem, Op::EventWrite}) {
        CHECK(StreamReuse::KeepsWindow(op));
    }
    for (const Op op : {Op::WriteData, Op::WaitRegMem, Op::EventWriteEop, Op::EventWriteEos,
                        Op::ReleaseMem, Op::DmaData, Op::CopyData,
                        Op::DumpConstRam, Op::WaitOnCeCounter, Op::IncrementDeCounter,
                        Op::MemSemaphore, Op::Rewind, Op::StrmoutBufferUpdate, Op::AtomicGds,
                        Op::Atomic, Op::CondWrite, Op::OcclusionQuery, Op::GetLodStats}) {
        CHECK(!StreamReuse::KeepsWindow(op));
    }
    CHECK(!StreamReuse::KeepsWindow(static_cast<Op>(0xff))); // unknown packets end it
    const u64 before = StreamReuse::window.load();
    StreamReuse::EndWindow(Op::WriteData);
    CHECK(StreamReuse::window.load() == before + 1);
    CHECK(StreamReuse::packet_ends[static_cast<u32>(Op::WriteData)].load() >= 1);
}

// Random draws over a few hot ranges (per-frame constants) and many one-off ones, with guest
// writes after labels or submissions, host writes, submits and ring wraps in between.
void TestRandom() {
    Model m;
    std::mt19937_64 rng{0x5eed};
    std::array<std::pair<u64, u32>, 6> hot{};
    for (auto& [address, size] : hot) {
        size = static_cast<u32>(64 * (1 + rng() % 32));
        address = (rng() % (MemorySize - size)) & ~u64{3};
    }
    for (u64 step = 0; step < 200000; ++step) {
        const u64 roll = rng() % 1000;
        if (roll < 800) {
            const auto [address, size] = hot[rng() % hot.size()];
            m.BindAndCheck(address, size);
        } else if (roll < 960) {
            const u32 size = static_cast<u32>(4 * (1 + rng() % 128));
            m.BindAndCheck((rng() % (MemorySize - size)) & ~u64{3}, size);
        } else if (roll < 970) {
            const auto [address, size] = hot[rng() % hot.size()];
            const u64 at = address + rng() % size;
            m.GuestWriteThenLabel(at, 1, static_cast<u8>(rng()), rng() & 1);
        } else if (roll < 980) {
            const auto [address, size] = hot[rng() % hot.size()];
            m.GuestWriteThenSubmission(address, size, static_cast<u8>(rng()));
        } else if (roll < 995) {
            const auto [address, size] = hot[rng() % hot.size()];
            m.HostWrite(address + rng() % size, 1, static_cast<u8>(rng()));
        } else {
            m.Submit();
        }
    }
    std::printf("random: copies=%llu reuses=%llu\n", static_cast<unsigned long long>(m.copies),
                static_cast<unsigned long long>(m.reuses));
    CHECK(m.reuses > m.copies); // the hot ranges are mostly reused
}

} // namespace

int main() {
    TestSameWindowReuses();
    TestDifferentRangeCopies();
    TestTickEndsWindow();
    TestGuestWriteAfterLabel();
    TestHostWrite();
    TestRingWrapSubmits();
    TestDisabled();
    TestPackets();
    TestRandom();
    std::printf("stream_reuse_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
