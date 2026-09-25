// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <iostream>
#include "core/diagnostics/overlay_control.h"
using namespace Core::Diagnostics;
int main() {
    OverlayMailbox box;
    assert(box.PixelDensity() == 1.f);
    box.SetPixelDensity(2.f);
    box.SetPixelDensity(NAN);
    box.SetPixelDensity(0.f);
    assert(box.PixelDensity() == 2.f);
    box.SetPixelDensity(20.f);
    assert(box.PixelDensity() == 4.f);
    std::vector<OverlayTouch> touch;
    std::vector<std::string> commands;
    assert(!box.Touch(1, 0, .5f, .5f));
    const auto first = box.Begin();
    box.Publish(first, {{.1f, .1f, .3f, .3f}});
    assert(!box.Touch(1, 0, .5f, .5f));
    assert(box.Touch(1, 0, .2f, .2f));
    assert(box.Touch(2, 0, .3f, .3f));
    assert(box.Touch(1, 1, .9f, .9f));  // Capture survives leaving the panel.
    assert(!box.Touch(3, 1, .2f, .2f)); // Never capture the tail of a game gesture.
    box.Drain(touch, commands);
    assert(touch.size() == 3 && !box.Pending());
    assert(box.Touch(1, 2, .9f, .9f));
    assert(!box.Touch(1, 1, .2f, .2f));
    assert(!box.Touch(4, 0, NAN, .2f));
    assert(!box.Touch(4, 4, .2f, .2f));
    for (int i = 0; i < 300; ++i)
        assert(box.Touch(2, 1, .2f, .2f));
    box.Drain(touch, commands);
    assert(touch.size() <= 256 && touch.front().id == -1);
    assert(box.Touch(2, 2, .2f, .2f));
    const auto second = box.Begin();
    box.End(first); // Old owner cannot tear down its replacement.
    box.Publish(first, {{0, 0, 1, 1}});
    assert(!box.Touch(5, 0, .2f, .2f));
    box.Publish(second, {{0, 0, 1, 1}});
    assert(box.Touch(5, 0, .2f, .2f));
    box.Cancel();
    box.Drain(touch, commands);
    assert(touch.size() == 1 && touch[0].id == -1);
    assert(!box.Touch(5, 1, .2f, .2f));
    for (int i = 0; i < 40; ++i)
        box.Request(std::to_string(i));
    box.Drain(touch, commands);
    assert(commands.size() == 32 && commands.front() == "8" && commands.back() == "39");
    box.End(second);
    assert(!box.Touch(6, 0, .2f, .2f) && !box.Pending());
    std::cout << "Overlay mailbox: capture, multitouch, cancellation, bounded queues and stale "
                 "owner passed\n";
}
