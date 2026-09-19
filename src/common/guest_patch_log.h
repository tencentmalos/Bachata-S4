// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace Common::GuestPatchLog {
// Writes to shadPS4's dedicated guest-patch sink/file. It shares the main
// logger lifecycle while remaining outside the main logger/filter/console.
void Write(std::string_view package, std::string_view tag, int64_t value, uint64_t context,
           uint64_t thread, uint64_t generation, uint64_t invocation) noexcept;
} // namespace Common::GuestPatchLog
