// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Common {
// Positive fractional milliseconds must still wait; avoid addition overflow.
constexpr int PollTimeoutMilliseconds(int microseconds) {
    return microseconds <= 0 ? microseconds : microseconds / 1000 + (microseconds % 1000 != 0);
}
} // namespace Common
