// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "core/signals.h"

#if defined(__ANDROID__)
#include <android/log.h>
// SIGTRAP does not reliably produce an Android ApplicationExitInfo tombstone.
// Preserve a debuggerd stack even when asynchronous host logs have not drained.
#define Crash()                                                                                    \
    __android_log_assert(nullptr, "shadps4", "Host assertion failed; see native backtrace")
#elif defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

void assert_fail_impl() {
#if !defined(__ANDROID__)
    Core::Signals::Instance()->RemoveHandlers();
#endif
    Crash();
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
