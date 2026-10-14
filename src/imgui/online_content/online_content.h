// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace OnlineContent {
// Launch-only integration: no download engine, accounts or content SDK in the emulator.
bool Open();
class Page {
public:
    void Draw();
private:
    bool launch_failed = false;
};
}
