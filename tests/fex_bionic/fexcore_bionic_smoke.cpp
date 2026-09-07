// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal FEXCore-on-bionic smoke probe.
//
// Answers one question the host-only tests cannot: does a FEXCore built against the Android NDK
// sysroot actually link and initialise inside a bionic process on a device?
//
// Deliberately narrow. It reports the host page size FEXCore was told to use, creates a Context and
// a guest thread, and tears both down. It does NOT execute guest code, so a pass here is evidence
// of link and lifecycle only, not of translation or JIT correctness.

#include "Common/HostFeatures.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {
int Failures = 0;
void Check(const char* Case, const char* Name, bool Condition, const char* Detail = "") {
  printf("[%-6s] %s %s", Case, Name, Condition ? "PASS" : "FAIL");
  if (Detail && Detail[0]) {
    printf(" -- %s", Detail);
  }
  printf("\n");
  fflush(stdout);
  if (!Condition) {
    ++Failures;
  }
}

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  printf("  (fex %s) %s\n", LogMan::DebugLevelStr(Level), Message);
  fflush(stdout);
}

void AssertHandler(const char* Message) {
  printf("  (fex assert) %s\n", Message);
  fflush(stdout);
}

// InitCore calls SignalDelegation->SetConfig unconditionally, so a context without a delegator
// dereferences null there. FEXCore::SignalDelegator is concrete -- it only stores the config the
// dispatcher hands it -- so an embedder that installs its own signal handling can supply this much
// and defer real delegation. Nothing here executes guest code, so no signal is ever delivered.
class MinimalSignalDelegator final : public FEXCore::SignalDelegator {
public:
  uintptr_t GetThunkCallbackRET() const override {
    return 0;
  }
};

// LookupCache's constructor calls CTX->SyscallHandler->MarkOvercommitRange, so CreateThread
// dereferences null without one. Three methods are pure virtual and must be supplied even by an
// embedder that dispatches syscalls itself; the rest have usable defaults.
class MinimalSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    // Unreachable here: no guest code runs, so no syscall is ever raised. Say so loudly rather than
    // silently returning, which would look like a handled syscall if this ever did get called.
    ERROR_AND_DIE_FMT("Guest syscall raised in a probe that executes no guest code");
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Address) override {
    // Nothing is mapped executable. Report an empty, non-writable range instead of claiming the
    // address is valid code.
    return {.Base = Address, .Size = 0, .Writable = false};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState* Thread,
                                                                               uint64_t GuestAddr) override {
    return std::nullopt;
  }
};
} // namespace

int main() {
  const long ReportedPage = sysconf(_SC_PAGESIZE);
  printf("bionic FEXCore smoke\n");
  printf("sysconf(_SC_PAGESIZE) = %ld\n", ReportedPage);
  printf("FEX_PAGE_SIZE         = %zu (guest ABI, must stay 4096)\n", FEXCore::Utils::FEX_PAGE_SIZE);
  fflush(stdout);

  Check("F01a", "FEX_PAGE_SIZE is still the guest ABI page", FEXCore::Utils::FEX_PAGE_SIZE == 4096);

  // Before anything else: FEXCore has no sysconf call of its own, so the host page size has to be
  // injected. Everything below depends on this having been accepted.
  const bool Accepted = FEXCore::Utils::SetHostPageSize(static_cast<size_t>(ReportedPage));
  char Detail[128];
  snprintf(Detail, sizeof(Detail), "reported=%ld accepted=%d HostPageSize()=%zu", ReportedPage, static_cast<int>(Accepted),
           FEXCore::Utils::HostPageSize());
  Check("F01b", "host page size injected into FEXCore", Accepted && FEXCore::Utils::HostPageSize() == static_cast<size_t>(ReportedPage),
        Detail);
  if (!Accepted) {
    // A host page this build cannot support: stop rather than proceed with a stale 4096 and produce
    // a misleading pass below.
    printf("\nFAILED: this build refuses host page %ld; see FEX_MAX_HOST_PAGE_SIZE\n", ReportedPage);
    return 1;
  }

  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEXCore::Config::Initialize();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::ReloadMetaLayer();
  Check("F02a", "FEXCore config layer initialises", true);

  // Allocator hooks want the host page size too; this is the existing FEXCore entry point for it.
  FEXCore::Allocator::SetupHooks(static_cast<size_t>(ReportedPage));
  Check("F02b", "allocator hooks accept the host page size", true);

  // Reads the real ID registers on this device rather than assuming a feature set.
  auto HostFeatures = FEX::FetchHostFeatures();
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  Check("F03a", "Context::CreateNewContext returns a context", !!CTX);
  if (!CTX) {
    printf("\nFAILED: no context\n");
    return 1;
  }

  // Must precede InitCore: it dereferences the delegator to install the dispatcher's config.
  MinimalSignalDelegator SignalDelegation;
  CTX->SetSignalDelegator(&SignalDelegation);

  // Must precede CreateThread: LookupCache's constructor calls into the syscall handler.
  MinimalSyscallHandler SyscallHandler;
  CTX->SetSyscallHandler(&SyscallHandler);
  Check("F03b", "signal delegator and syscall handler installed", true);

  // InitCore is the call the previous round could not reach on a 16 KiB host: it is what allocates
  // the JIT code buffer whose guard page was one of the two blockers.
  const bool Inited = CTX->InitCore();
  Check("F03c", "InitCore succeeds", Inited);
  if (!Inited) {
    printf("\nFAILED: InitCore returned false\n");
    return 1;
  }

  FEXCore::Core::CPUState State {};
  // A plausible-looking but never-executed entry state. CreateThread must not require the address
  // to be mapped, since nothing runs here.
  State.rip = 0x1000;
  State.gregs[FEXCore::X86State::REG_RSP] = 0x2000;

  auto* Thread = CTX->CreateThread(&State);
  Check("F04a", "CreateThread returns a thread state", !!Thread);

  if (Thread) {
    // The fault page must be host-page aligned for the mprotect in DestroyThread to succeed. Report
    // the alignment directly: on a 16 KiB host the old layout landed at offset 8192 within the page.
    auto FaultAddr = reinterpret_cast<uintptr_t>(&Thread->InterruptFaultPage);
    snprintf(Detail, sizeof(Detail), "fault page addr%%%zu=%zu size=%zu", FEXCore::Utils::HostPageSize(),
             static_cast<size_t>(FaultAddr % FEXCore::Utils::HostPageSize()), sizeof(Thread->InterruptFaultPage));
    Check("F04b", "live InterruptFaultPage is host-page aligned", FaultAddr % FEXCore::Utils::HostPageSize() == 0, Detail);

    // DestroyThread is the second blocker's site: it mprotects InterruptFaultPage back to RW before
    // freeing. With the old layout on a 16 KiB host that mprotect failed; it is now fatal, so
    // reaching the next line at all is the evidence.
    CTX->DestroyThread(Thread);
    Check("F04c", "DestroyThread restores fault page permissions and frees", true);
  }

  CTX.reset();
  Check("F05a", "context destruction completes", true);

  FEXCore::Allocator::ClearHooks();
  FEXCore::Config::Shutdown();
  Check("F05b", "allocator hooks and config shut down", true);

  printf("\n%s (%d failure%s)\n", Failures == 0 ? "ALL PASS" : "FAILED", Failures, Failures == 1 ? "" : "s");
  printf("SCOPE: link + init + thread lifecycle on bionic. No guest code was executed,\n");
  printf("       so this does not prove translation, JIT or guest ABI correctness.\n");
  return Failures == 0 ? 0 : 1;
}
