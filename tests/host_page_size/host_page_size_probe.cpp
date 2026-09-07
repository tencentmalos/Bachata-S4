// SPDX-License-Identifier: MIT
//
// Host page size regression probe.
//
// Verifies the two 4 KiB assumptions on the embedder's mandatory path actually behave on a host
// whose page size exceeds FEX_PAGE_SIZE:
//   1. The JIT code buffer guard page is host-page aligned and sized, so mprotect succeeds and an
//      overrun faults instead of silently corrupting the heap.
//   2. InterruptFaultPage is host-page aligned, so the dispatcher interrupt mechanism can actually
//      be protected -- pause and deferred signal handling are built on it.
//
// This reproduces the layout and calls rather than linking FEXCore so it can run standalone on the
// build host. It fails on a 4 KiB host too (where it trivially passes), so it is safe to run
// anywhere; it only proves something on a host with a larger page.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace {
// Mirrors FEXCore::Utils from TypeDefines.h.
constexpr size_t FEX_PAGE_SIZE = 4096;
constexpr size_t FEX_MAX_HOST_PAGE_SIZE = 16384;

size_t HostPageSizeValue = FEX_PAGE_SIZE;
size_t HostPageSize() {
  return HostPageSizeValue;
}

template<typename T>
T AlignDown(T value, size_t alignment) {
  return value & ~static_cast<T>(alignment - 1);
}

int Failures = 0;
// Emits the "[case] name PASS/FAIL -- detail" form that scripts/android/run-v0-tests parses, so
// these results feed the acceptance matrix instead of living only in this program's stdout.
void Check(const char* Case, const char* Name, bool Condition, const char* Detail = "") {
  printf("[%-6s] %s %s", Case, Name, Condition ? "PASS" : "FAIL");
  if (Detail && Detail[0]) {
    printf(" -- %s", Detail);
  }
  printf("\n");
  if (!Condition) {
    ++Failures;
  }
}

// ---------------------------------------------------------------------------
// 1. Code buffer guard page.
// ---------------------------------------------------------------------------
void TestCodeBufferGuard() {
  // Matches SharedCodeBufferManager's INITIAL_CODE_SIZE.
  constexpr size_t Size = 1024 * 1024 * 16;
  const size_t GuardSize = HostPageSize();

  auto* Ptr = static_cast<uint8_t*>(mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Ptr == MAP_FAILED) {
    Check("P01a", "code buffer allocation", false, strerror(errno));
    return;
  }

  uintptr_t GuardAddr = AlignDown(reinterpret_cast<uintptr_t>(Ptr) + Size - 1, GuardSize);

  Check("P01a", "guard page start is host-page aligned", GuardAddr % HostPageSize() == 0);
  Check("P01b", "guard page stays inside the allocation", GuardAddr + GuardSize <= reinterpret_cast<uintptr_t>(Ptr) + Size);
  Check("P01c", "guard page covers the buffer tail exactly", GuardAddr + GuardSize == reinterpret_cast<uintptr_t>(Ptr) + Size);

  errno = 0;
  bool Protected = mprotect(reinterpret_cast<void*>(GuardAddr), GuardSize, PROT_NONE) == 0;
  Check("P01d", "mprotect on the guard page succeeds", Protected, Protected ? "" : strerror(errno));

  // The old code computed the guard with FEX_PAGE_SIZE. On a larger host that address is unaligned
  // and mprotect must reject it -- this is the failure the fix removes. Assert the old behaviour
  // really was broken, so the test would catch a regression back to it.
  if (HostPageSize() > FEX_PAGE_SIZE) {
    uintptr_t OldAddr = AlignDown(reinterpret_cast<uintptr_t>(Ptr) + Size - 1, FEX_PAGE_SIZE);
    Check("P01e", "the previous FEX_PAGE_SIZE guard address was indeed unaligned", OldAddr % HostPageSize() != 0);
    errno = 0;
    bool OldWouldFail = mprotect(reinterpret_cast<void*>(OldAddr), FEX_PAGE_SIZE, PROT_NONE) != 0;
    Check("P01f", "the previous computation would have failed here", OldWouldFail,
          OldWouldFail ? strerror(errno) : "unexpectedly succeeded");
  }

  // UsableSize() must exclude the whole guard, otherwise code would be emitted into protected pages.
  size_t UsableSize = Size - GuardSize;
  Check("P01g", "UsableSize() ends at or before the guard", reinterpret_cast<uintptr_t>(Ptr) + UsableSize <= GuardAddr);

  munmap(Ptr, Size);
}

// ---------------------------------------------------------------------------
// 2. InterruptFaultPage alignment.
// ---------------------------------------------------------------------------
struct AllocOperators {
  void* operator new(size_t size) {
    return malloc(size);
  }
  void* operator new(size_t size, std::align_val_t align) {
    return aligned_alloc(static_cast<size_t>(align), size);
  }
  void operator delete(void* ptr) {
    free(ptr);
  }
  void operator delete(void* ptr, std::align_val_t) {
    free(ptr);
  }
};

// Stand-in for CpuStateFrame; only its presence directly before the fault page matters here.
struct FrameLike {
  uint8_t Opaque[3000];
};

// The fixed layout: object and fault page aligned to the maximum supported host page.
struct alignas(FEX_MAX_HOST_PAGE_SIZE) ThreadStateLike : public AllocOperators {
  uint8_t Leading[512];
  FrameLike BaseFrameState {};
  alignas(FEX_MAX_HOST_PAGE_SIZE) uint8_t InterruptFaultPage[FEX_MAX_HOST_PAGE_SIZE];
};

// The previous layout, for contrast.
struct alignas(FEX_PAGE_SIZE) OldThreadStateLike : public AllocOperators {
  uint8_t Leading[512];
  FrameLike BaseFrameState {};
  alignas(FEX_PAGE_SIZE) uint8_t InterruptFaultPage[FEX_PAGE_SIZE];
};

void TestInterruptFaultPage() {
  // JIT encodes this distance as a store immediate; see Arm64JITCore::EmitSuspendInterruptCheck.
  constexpr size_t Distance = offsetof(ThreadStateLike, InterruptFaultPage) - offsetof(ThreadStateLike, BaseFrameState);
  char Detail[128];
  snprintf(Detail, sizeof(Detail), "distance=%zu", Distance);
  Check("P02a", "fault page stays within the JIT immediate offset budget", Distance <= 65520, Detail);

  // aligned_alloc rejects a size that is not a multiple of the alignment on some platforms.
  Check("P02b", "object size is a multiple of its alignment", sizeof(ThreadStateLike) % alignof(ThreadStateLike) == 0);

  auto* State = new ThreadStateLike();
  auto FaultAddr = reinterpret_cast<uintptr_t>(&State->InterruptFaultPage);

  snprintf(Detail, sizeof(Detail), "addr%%%zu=%zu", HostPageSize(), static_cast<size_t>(FaultAddr % HostPageSize()));
  Check("P02c", "fault page is host-page aligned", FaultAddr % HostPageSize() == 0, Detail);
  Check("P02d", "fault page covers at least one host page", sizeof(State->InterruptFaultPage) >= HostPageSize());

  errno = 0;
  bool ProtectedNone = mprotect(reinterpret_cast<void*>(FaultAddr), sizeof(State->InterruptFaultPage), PROT_NONE) == 0;
  Check("P02e", "mprotect(PROT_NONE) on the fault page succeeds", ProtectedNone, ProtectedNone ? "" : strerror(errno));

  errno = 0;
  bool Restored = mprotect(reinterpret_cast<void*>(FaultAddr), sizeof(State->InterruptFaultPage), PROT_READ | PROT_WRITE) == 0;
  Check("P02f", "permissions can be restored before free", Restored, Restored ? "" : strerror(errno));

  // Protecting the fault page must not strip permissions from BaseFrameState, which the JIT keeps
  // writing while the interrupt is armed. Verify they are in different host pages.
  auto FrameAddr = reinterpret_cast<uintptr_t>(&State->BaseFrameState);
  Check("P02g", "BaseFrameState is in a different host page than the fault page",
        AlignDown(FrameAddr, HostPageSize()) != AlignDown(FaultAddr, HostPageSize()));

  delete State;

  // Contrast: the old layout is not host-page aligned once the host page exceeds 4096.
  if (HostPageSize() > FEX_PAGE_SIZE) {
    auto* Old = new OldThreadStateLike();
    auto OldFaultAddr = reinterpret_cast<uintptr_t>(&Old->InterruptFaultPage);
    Check("P02h", "the previous layout was indeed misaligned", OldFaultAddr % HostPageSize() != 0);
    errno = 0;
    bool OldWouldFail = mprotect(reinterpret_cast<void*>(OldFaultAddr), sizeof(Old->InterruptFaultPage), PROT_NONE) != 0;
    Check("P02i", "the previous layout's mprotect would have failed", OldWouldFail,
          OldWouldFail ? strerror(errno) : "unexpectedly succeeded");
    delete Old;
  }
}

// ---------------------------------------------------------------------------
// 3. SetHostPageSize validation.
// ---------------------------------------------------------------------------
bool SetHostPageSizeModel(size_t Size) {
  if (Size < FEX_PAGE_SIZE || (Size & (Size - 1)) != 0) {
    return false;
  }
  if (Size > FEX_MAX_HOST_PAGE_SIZE) {
    return false;
  }
  HostPageSizeValue = Size;
  return true;
}

void TestSetHostPageSize() {
  size_t Saved = HostPageSizeValue;

  Check("P03a", "rejects zero", !SetHostPageSizeModel(0));
  Check("P03b", "rejects a page smaller than the guest ABI page", !SetHostPageSizeModel(1024));
  Check("P03c", "rejects a non-power-of-two page", !SetHostPageSizeModel(6144));
  // 64k would push the fault page past the JIT immediate budget; it must be refused, not accepted.
  Check("P03d", "rejects 64k, which exceeds the JIT offset budget", !SetHostPageSizeModel(65536));
  Check("P03e", "accepts 4096", SetHostPageSizeModel(4096));
  Check("P03f", "accepts 16384", SetHostPageSizeModel(16384));

  HostPageSizeValue = Saved;
}
} // namespace

int main() {
  const long Reported = sysconf(_SC_PAGESIZE);
  HostPageSizeValue = Reported > 0 ? static_cast<size_t>(Reported) : FEX_PAGE_SIZE;

  printf("host page size reported by sysconf: %ld\n", Reported);
  if (HostPageSizeValue <= FEX_PAGE_SIZE) {
    printf("NOTE: running on a %zu byte page host; the misalignment cases below cannot be exercised here.\n", HostPageSizeValue);
  }
  if (HostPageSizeValue > FEX_MAX_HOST_PAGE_SIZE) {
    printf("NOTE: host page %zu exceeds FEX_MAX_HOST_PAGE_SIZE %zu; this build would refuse to run here.\n", HostPageSizeValue,
           FEX_MAX_HOST_PAGE_SIZE);
    return 1;
  }
  printf("\n-- code buffer guard --\n");
  TestCodeBufferGuard();
  printf("\n-- interrupt fault page --\n");
  TestInterruptFaultPage();
  printf("\n-- SetHostPageSize validation --\n");
  TestSetHostPageSize();

  printf("\n%s (%d failure%s)\n", Failures == 0 ? "ALL PASS" : "FAILED", Failures, Failures == 1 ? "" : "s");
  return Failures == 0 ? 0 : 1;
}
