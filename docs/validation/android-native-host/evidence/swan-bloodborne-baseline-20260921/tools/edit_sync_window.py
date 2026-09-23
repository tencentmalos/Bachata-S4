"""Make the guest mutex fast path's arena window host-published instead of a compile-time constant.

Root cause (Swan run R, 2026-09-22): ServiceAllocationBase moved to 112 GiB (PSVR memory work) while
guest_sync_abi.h still hard-coded the arena window at 64 GiB, so every GuestSyncObjects block landed
outside the window and the payload sent every mutex op to HLE (2600 lock+unlock HLE calls/frame on
Guest-1, ~36 ms/frame). The window is now a 256 MiB reservation whose bounds the host writes into the
payload's `shad_sync_window` table at publication.
"""
import os, re, sys
os.chdir(r"C:/workspace/emulations/shadps4")

def edit(path, pairs, must_all=True):
    s = open(path, encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in s else "\n"
    for old, new in pairs:
        old_n, new_n = old.replace("\n", nl), new.replace("\n", nl)
        if old_n not in s:
            if must_all:
                raise SystemExit(f"anchor missing in {path}:\n{old[:200]}")
            continue
        assert s.count(old_n) == 1, f"anchor not unique in {path}: {old[:80]}"
        s = s.replace(old_n, new_n)
    open(path, "w", encoding="utf-8", newline="").write(s)
    print("edited", path)

# 1. Shared ABI: window table instead of fixed constants.
edit("src/core/host_runtime/guest_sync_abi.h", [(
"""/* Slot values below this are static initializers (0/1) or destroyed (2); real
 * objects live inside [ARENA_BASE, ARENA_LIMIT). Anything else goes to HLE. */
#define SHAD_SYNC_ARENA_BASE 0x1000000000ull
#define SHAD_SYNC_ARENA_LIMIT 0x1010000000ull
""",
"""/* Slot values 0/1 are static initializers and 2 is destroyed; real objects live
 * inside the arena window [base, limit) that the host reserves and writes into
 * the payload's `shad_sync_window` table at publication. The window is NOT a
 * compile-time address: the service allocation base already moved once
 * (64 GiB -> 112 GiB) and silently sent every mutex back to HLE. base == limit
 * == 0 means no window: every object takes the HLE path. */
#define SHAD_SYNC_ARENA_WINDOW_SIZE 0x10000000ull /* 256 MiB reserved by the host */
enum ShadSyncWindow {
    ShadSyncWindowBase = 0,  /* u64: first arena object address */
    ShadSyncWindowLimit = 1, /* u64: one past the last arena object address */
    ShadSyncWindowCount = 2
};
"""), (
"""#define SHAD_SYNC_IMPORT_TABLE "shad_sync_imports"
""",
"""#define SHAD_SYNC_IMPORT_TABLE "shad_sync_imports"
#define SHAD_SYNC_WINDOW_TABLE "shad_sync_window"
""")])

# 2. Guest payload: read the window from the host-filled table.
edit("guest/runtime/sync/mutex.c", [(
"""__attribute__((section(".shad_imports"), used, visibility("default")))
const volatile u64 shad_sync_imports[ShadSyncImportCount];
""",
"""__attribute__((section(".shad_imports"), used, visibility("default")))
const volatile u64 shad_sync_imports[ShadSyncImportCount];
/* Host-filled arena window [base, limit) of real mutex objects; both zero until
 * the host reserved a window, which sends every object to the HLE path. */
__attribute__((section(".shad_imports"), used, visibility("default")))
const volatile u64 shad_sync_window[ShadSyncWindowCount];
"""), (
"""static inline struct Mutex* Object(u64 address) {
    /* Static initializers (0/1), destroyed (2) and anything outside the arena
     * take the checked HLE path, which also performs lazy initialization. */
    if (address - SHAD_SYNC_ARENA_BASE >= SHAD_SYNC_ARENA_LIMIT - SHAD_SYNC_ARENA_BASE)
        return 0;
    return (struct Mutex*)address;
}
""",
"""static inline struct Mutex* Object(u64 address) {
    /* Static initializers (0/1), destroyed (2) and anything outside the
     * host-published arena window take the checked HLE path, which also
     * performs lazy initialization. Two RIP-relative loads; no HLE crossing. */
    const u64 base = shad_sync_window[ShadSyncWindowBase];
    if (address - base >= shad_sync_window[ShadSyncWindowLimit] - base)
        return 0;
    return (struct Mutex*)address;
}
""")])

# 3. Host runtime: reserve the window, carve arena blocks from it, publish bounds.
edit("src/core/host_runtime/guest_runtime.cpp", [(
"""    std::atomic<unsigned> sync_arena_outside{};
""",
"""    std::atomic<unsigned> sync_arena_outside{};
    // Arena object window [base, limit): one reservation carved into 16 KiB
    // blocks; its bounds are what the payload's `shad_sync_window` table gets.
    u64 sync_window_base{}, sync_window_limit{}, sync_window_next{};
"""), (
"""    sync_arena = std::make_unique<GuestSyncArena>([this] {
        void* address{};
        const auto result = memory->MapMemory(
            &address, GuestRuntime::ServiceAllocationBase, GuestSyncArena::BlockSize, MemoryProt::CpuReadWrite,
            MemoryMapFlags::NoFlags, VMAType::File, "GuestSyncObjects");
        if (result == ORBIS_KERNEL_ERROR_ENOMEM) return u64{0};
        if (result) throw std::runtime_error("GuestSyncObjects mapping failed");
        const auto block = reinterpret_cast<u64>(address);
        // The guest fast path only touches objects inside the declared arena
        // window; a block placed elsewhere stays correct on the HLE path.
        if (block < SHAD_SYNC_ARENA_BASE || block + GuestSyncArena::BlockSize > SHAD_SYNC_ARENA_LIMIT)
            if (sync_arena_outside.fetch_add(1, std::memory_order_relaxed) == 0)
                LOG_WARNING(Core_Linker, "GuestSyncObjects block {:#x} is outside the fast-path arena window",
                            block);
        return block;
    });
""",
"""    // The guest fast path only touches objects inside one arena window whose
    // bounds it learns at publication (InstallSyncFastPath). Reserve that window
    // here, before any object exists: address space only, pages are committed as
    // objects are created, and its placement follows the memory manager rather
    // than a hard-coded address (the service base already moved 64 -> 112 GiB
    // once and silently parked every mutex on the HLE path).
    try {
        sync_window_base = Allocate(SHAD_SYNC_ARENA_WINDOW_SIZE, "GuestSyncObjects");
        sync_window_limit = sync_window_base + SHAD_SYNC_ARENA_WINDOW_SIZE;
        sync_window_next = sync_window_base;
    } catch (const std::exception& e) {
        LOG_WARNING(Core_Linker, "GuestSyncObjects window unavailable ({}); mutex fast path stays on HLE",
                    e.what());
    }
    sync_arena = std::make_unique<GuestSyncArena>([this] {
        if (sync_window_base && sync_window_next + GuestSyncArena::BlockSize <= sync_window_limit) {
            const auto block = sync_window_next; // fresh zero pages of the reservation
            sync_window_next += GuestSyncArena::BlockSize;
            return block;
        }
        void* address{};
        const auto result = memory->MapMemory(
            &address, GuestRuntime::ServiceAllocationBase, GuestSyncArena::BlockSize, MemoryProt::CpuReadWrite,
            MemoryMapFlags::NoFlags, VMAType::File, "GuestSyncObjects");
        if (result == ORBIS_KERNEL_ERROR_ENOMEM) return u64{0};
        if (result) throw std::runtime_error("GuestSyncObjects mapping failed");
        const auto block = reinterpret_cast<u64>(address);
        // Outside the published window: still correct on the HLE path, but every
        // operation on these objects pays a crossing again.
        if (sync_arena_outside.fetch_add(1, std::memory_order_relaxed) == 0)
            LOG_WARNING(Core_Linker,
                        "GuestSyncObjects block {:#x} is outside the fast-path arena window [{:#x}, {:#x}): {}",
                        block, sync_window_base, sync_window_limit,
                        sync_window_base ? "window exhausted" : "no window reserved");
        return block;
    });
"""), (
"""    if (memory->Protect(sync_fastpath_base, sync_fastpath_size, MemoryProt::CpuRead | MemoryProt::CpuExec))
        throw std::runtime_error("guest sync payload publication failed");
""",
"""    // Arena window bounds: the payload's only notion of "real object" addresses.
    const auto window = symbol(SHAD_SYNC_WINDOW_TABLE);
    if (window.size != ShadSyncWindowCount * sizeof(u64) || window.offset % 8)
        throw std::runtime_error("guest sync payload window table mismatch");
    const std::array<u64, ShadSyncWindowCount> bounds{sync_window_base, sync_window_limit};
    for (unsigned i = 0; i < ShadSyncWindowCount; ++i) {
        u64 slot{};
        std::memcpy(&slot, reinterpret_cast<void*>(sync_fastpath_base + window.offset + 8 * i), 8);
        if (slot) throw std::runtime_error("guest sync payload window slot is not empty");
        std::memcpy(reinterpret_cast<void*>(sync_fastpath_base + window.offset + 8 * i), &bounds[i], 8);
    }
    if (memory->Protect(sync_fastpath_base, sync_fastpath_size, MemoryProt::CpuRead | MemoryProt::CpuExec))
        throw std::runtime_error("guest sync payload publication failed");
"""), (
"""    sync_fastpath_status = "installed";
    LOG_INFO(Core_Linker, "Guest sync fast path installed: base={:#x} bytes={} image_sha256={}",
             sync_fastpath_base, sizeof(Image), ImageSha256);
""",
"""    // "installed" only when objects can actually be reached from the guest.
    sync_fastpath_status = sync_window_base ? "installed" : "installed_no_arena_window";
    LOG_INFO(Core_Linker,
             "Guest sync fast path {}: base={:#x} bytes={} image_sha256={} arena_window=[{:#x}, {:#x})",
             sync_fastpath_status, sync_fastpath_base, sizeof(Image), ImageSha256, sync_window_base,
             sync_window_limit);
""")])

# 4. Test harness: pick a window of its own and publish it exactly as the runtime does.
edit("tests/host_runtime/guest_sync_fastpath_tests.cpp", [(
"""struct Harness {
""",
"""// The harness's own arena window; the payload learns it through the window table.
constexpr uint64_t kArenaBase = 0x1000000000ull;
constexpr uint64_t kArenaLimit = kArenaBase + SHAD_SYNC_ARENA_WINDOW_SIZE;

struct Harness {
"""), (
"""        // Place the reservation so the production arena window
        // [SHAD_SYNC_ARENA_BASE, LIMIT) lies inside it: the payload only takes
        // its fast path for objects inside that window.
        cfg.reservation_size = 0x20000000;
        cfg.preferred_base = SHAD_SYNC_ARENA_BASE - 0x10000000;
""",
"""        // Place the reservation so the arena window [kArenaBase, kArenaLimit)
        // lies inside it: the payload only takes its fast path for objects
        // inside the window the host publishes.
        cfg.reservation_size = 0x20000000;
        cfg.preferred_base = kArenaBase - 0x10000000;
"""), (
"""        arena_next = SHAD_SYNC_ARENA_BASE;
""",
"""        arena_next = kArenaBase;
"""), (
"""        for (unsigned i = 0; i < ShadSyncImportCount; ++i)
            std::memcpy(image.data() + table + 8 * i, &veneers[i], 8);
""",
"""        for (unsigned i = 0; i < ShadSyncImportCount; ++i)
            std::memcpy(image.data() + table + 8 * i, &veneers[i], 8);
        const auto window = Offset(SHAD_SYNC_WINDOW_TABLE);
        const uint64_t bounds[ShadSyncWindowCount] = {kArenaBase, kArenaLimit};
        for (unsigned i = 0; i < ShadSyncWindowCount; ++i)
            std::memcpy(image.data() + window + 8 * i, &bounds[i], 8);
"""), (
"""                    (unsigned long long)h.code, (unsigned long long)SHAD_SYNC_ARENA_BASE);
""",
"""                    (unsigned long long)h.code, (unsigned long long)kArenaBase);
"""), (
"""                  h.imports[ShadSyncImportMutexLock]->calls == 1 && object >= SHAD_SYNC_ARENA_BASE &&
                  object < SHAD_SYNC_ARENA_LIMIT && h.Read<uint64_t>(h.Counter(0)) == 1);
""",
"""                  h.imports[ShadSyncImportMutexLock]->calls == 1 && object >= kArenaBase &&
                  object < kArenaLimit && h.Read<uint64_t>(h.Counter(0)) == 1);
""")])

# 5. README wording.
edit("guest/runtime/sync/README.md", [(
"""`pthread_self` returns. Slot values below the arena window (0/1 static
initializers, 2 destroyed) or outside it take the HLE fallback imports, which
initialize lazily or report EINVAL exactly as before.
""",
"""`pthread_self` returns. Slot values outside the arena window (0/1 static
initializers and 2 destroyed always are) take the HLE fallback imports, which
initialize lazily or report EINVAL exactly as before. The window is not a
compile-time address: the host reserves 256 MiB (`GuestSyncObjects`,
`SHAD_SYNC_ARENA_WINDOW_SIZE`) next to its other service allocations, carves the
16 KiB arena blocks out of it, and writes `[base, limit)` into the payload's
`shad_sync_window` table before the image becomes executable. A payload that
hard-coded the window went inert when the service base moved from 64 GiB to
112 GiB (every mutex silently returned to HLE, ~2600 lock/unlock crossings per
frame in Bloodborne); `hle_sync status` now reports `installed_no_arena_window`
if the reservation failed.
"""), (
"""| `src/core/host_runtime/guest_sync_abi.h` | Shared layout: prefix offsets, states, types, arena window, import slot order, symbol names. |
""",
"""| `src/core/host_runtime/guest_sync_abi.h` | Shared layout: prefix offsets, states, types, window/import table slot order, symbol names. |
""")])

for path, pat in (("src/core/host_runtime/guest_runtime.cpp", r"SHAD_SYNC_ARENA_(BASE|LIMIT)\b"),
                  ("guest/runtime/sync/mutex.c", r"SHAD_SYNC_ARENA_(BASE|LIMIT)\b"),
                  ("tests/host_runtime/guest_sync_fastpath_tests.cpp", r"SHAD_SYNC_ARENA_(BASE|LIMIT)\b")):
    if re.search(pat, open(path, encoding="utf-8").read()):
        raise SystemExit(f"stale constant still used in {path}")
print("ok")
