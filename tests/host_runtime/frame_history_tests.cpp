// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/diagnostics/frame_history.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
int main() {
    unsigned checks{};
    auto check = [&](bool ok) { ++checks; if (!ok) std::exit(1); };
    Core::Diagnostics::FrameHistory history;
    check(history.Fps(0) == 0);
    history.Record(0);
    check(history.Count() == 0);
    for (uint64_t i = 1; i <= 60; ++i) history.Record(i * 16666667);
    check(history.Count() == 60);
    check(std::abs(history.Fps(1000000020) - 60) < .001);
    check(std::abs(history.At(0).milliseconds - 16.666667) < .0001);
    history.Record(1); history.Record(1000000020);
    check(history.Count() == 60); // Duplicate/reversed observations do not invent frames.
    check(history.Fps(2100000020) == 0); // A stale chart never reports the old live FPS.
    history.Record(3000000020);
    check(std::abs(history.At(60).milliseconds - 2000) < .001); // Retain real stalls.
    for (uint64_t i = 1; i <= 1100; ++i) history.Record(3000000020 + i * 1000000);
    check(history.Count() == history.Capacity);
    check(history.At(0).end_ns == 3000000020 + 77 * 1000000);
    Core::Diagnostics::FrameHistory next_session;
    check(next_session.Count() == 0 && next_session.Fps(9999999999) == 0);
    std::cout << checks << " checks, 0 failures\n";
}
