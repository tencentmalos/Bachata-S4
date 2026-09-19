// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/network/http_error.h"

namespace Libraries::Http2 {
// Until an async operation has a real completion producer, reject it before
// acceptance. Returning OK without an event strands callers in WaitAsync.
// HTTP2 currently shares the desktop HTTP transport/error domain.
inline constexpr int UnavailableAsyncResult() {
    return ORBIS_HTTP_ERROR_NETWORK;
}
} // namespace Libraries::Http2
