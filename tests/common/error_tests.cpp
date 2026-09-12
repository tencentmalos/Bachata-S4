// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "common/error.h"

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) {
            ++failures;
            std::printf("FAIL %s\n", name);
        }
    };
    for (const auto code : {0, EACCES, EINVAL, ENOMEM, ENOENT}) {
        // GNU may return a static message; POSIX fills the caller's buffer.
        const std::string expected = std::strerror(code);
        check(Common::NativeErrorToString(code) == expected, "errno_text");
    }
    check(!Common::NativeErrorToString(-12345).empty(), "unknown_errno_handled");
#ifndef _WIN32
    const auto expected = Common::NativeErrorToString(EACCES);
    errno = EACCES;
    check(Common::GetLastErrorMsg() == expected, "last_errno");
#endif
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
