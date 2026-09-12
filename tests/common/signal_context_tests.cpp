// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Linux/Android ARM64 signal-context regressions against the production implementation.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <asm/sigcontext.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common/error.h"
#include "common/signal_context.h"

static volatile sig_atomic_t expected_write;
static void FaultHandler(int, siginfo_t*, void* context) {
    const bool good = Common::GetRip(context) != nullptr &&
                      Common::IsWriteError(context) == static_cast<bool>(expected_write);
    _exit(good ? 0 : 81);
}
static bool RealFault(bool write) {
    const auto pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        alarm(3);
        expected_write = write;
        struct sigaction action {};
        action.sa_sigaction = FaultHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, nullptr) != 0)
            _exit(82);
        const size_t page_size = sysconf(_SC_PAGESIZE);
        void* p =
            mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED || mprotect(p, page_size, write ? PROT_READ : PROT_NONE) != 0)
            _exit(83);
        if (write)
            *static_cast<volatile unsigned char*>(p) = 42;
        else {
            const auto byte = *static_cast<volatile unsigned char*>(p);
            (void)byte;
        }
        _exit(84); // Required access violation did not occur.
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
int main() {
    int total = 0, failed = 0;
    auto check = [&](bool ok, const char* name) {
        ++total;
        if (!ok)
            ++failed;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    };
    ucontext_t ctx{};
    ctx.uc_mcontext.pc = 0x12345678;
    check(Common::GetRip(&ctx) == reinterpret_cast<void*>(0x12345678), "host_pc");
    check(!Common::IsWriteError(&ctx), "terminator");
    auto* first = reinterpret_cast<_aarch64_ctx*>(ctx.uc_mcontext.__reserved);
    first->magic = ESR_MAGIC;
    first->size = sizeof(esr_context);
    auto* esr = reinterpret_cast<esr_context*>(first);
    esr->esr = 0x90000040;
    check(Common::IsWriteError(&ctx), "valid_esr_write");
    esr->esr = 0x90000000;
    check(!Common::IsWriteError(&ctx), "valid_esr_read");
    first->size = 8;
    check(!Common::IsWriteError(&ctx), "short_esr");
    first->size = 8192;
    check(!Common::IsWriteError(&ctx), "oversize_record");
    first->size = 0;
    check(!Common::IsWriteError(&ctx), "zero_size_record");
    std::memset(&ctx, 0, sizeof(ctx));
    first = reinterpret_cast<_aarch64_ctx*>(ctx.uc_mcontext.__reserved);
    first->magic = 0x1234;
    first->size = 16;
    esr = reinterpret_cast<esr_context*>(ctx.uc_mcontext.__reserved + 16);
    esr->head.magic = ESR_MAGIC;
    esr->head.size = sizeof(esr_context);
    esr->esr = 0x90000040;
    check(Common::IsWriteError(&ctx), "skip_unknown_record");
    // Do not reinterpret a 4-byte-aligned ESR as a naturally-aligned 64-bit record.
    std::memset(&ctx, 0, sizeof(ctx));
    first = reinterpret_cast<_aarch64_ctx*>(ctx.uc_mcontext.__reserved);
    first->magic = 0x1234;
    first->size = 12;
    esr_context bad{};
    bad.head.magic = ESR_MAGIC;
    bad.head.size = sizeof(bad);
    bad.esr = 0x90000040;
    std::memcpy(ctx.uc_mcontext.__reserved + 12, &bad, sizeof(bad));
    check(!Common::IsWriteError(&ctx), "reject_misaligned_record");
    const auto text = Common::NativeErrorToString(EACCES);
    check(!text.empty() && text.front() != '(', "errno_string");
    for (int n = 0; n < 20; ++n) {
        check(RealFault(false), "real_read_fault");
        check(RealFault(true), "real_write_fault");
    }
    std::printf("checks=%d failures=%d\n", total, failed);
    return failed ? 1 : 0;
}
