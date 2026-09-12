// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#endif

#include "common/error.h"

namespace Common {

std::string NativeErrorToString(int e) {
#ifdef _WIN32
    LPSTR err_str;

    DWORD res = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, e, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                               reinterpret_cast<LPSTR>(&err_str), 1, nullptr);
    if (!res) {
        return "(FormatMessageA failed to format error)";
    }
    std::string ret(err_str);
    LocalFree(err_str);
    return ret;
#else
    char err_str[255];
    // Use the actual declaration selected by libc's feature macros. Both GNU
    // (char*) and POSIX (int) signatures are available on bionic and glibc.
    return [](auto result, const char* buffer) -> std::string {
        if constexpr (std::is_same_v<decltype(result), int>) {
            return result == 0 ? std::string(buffer) : "(strerror_r failed to format error)";
        } else {
            return result != nullptr ? std::string(result) : "(strerror_r failed to format error)";
        }
    }(strerror_r(e, err_str, sizeof(err_str)), err_str);
#endif // _WIN32
}

std::string GetLastErrorMsg() {
#ifdef _WIN32
    return NativeErrorToString(GetLastError());
#else
    return NativeErrorToString(errno);
#endif
}

} // namespace Common
