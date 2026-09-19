// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/guest_patch_log.h"

#include <fmt/format.h>
#include "common/logging/log.h"

namespace Common::GuestPatchLog {
void Write(std::string_view package, std::string_view tag, int64_t value, uint64_t context,
           uint64_t thread, uint64_t generation, uint64_t invocation) noexcept {
    try {
        const auto message =
            fmt::format("[PATCH_LOG] package={} tag={} value={} context={} thread={} generation={} "
                        "invocation={}",
                        package, tag, value, context, thread, generation, invocation);
        Common::Log::WriteGuestPatch(message);
    } catch (...) {
        // Diagnostics must never affect guest execution or HLE return values.
    }
}
} // namespace Common::GuestPatchLog
