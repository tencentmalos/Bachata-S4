// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

// Android dynamic performance hints (ADPF, APerformanceHint): a hint session over chosen threads
// whose reported work duration is the interval between new game frames. Lets the platform raise
// the clocks or placement of threads the frame waits on. Experimental and off by default
// (DebugBus `perf_hint`); other platforms report it as unsupported.
namespace Common::PerformanceHint {

// on guest|<tid>,<tid>... [target_ms] | off | status
std::string Command(const std::vector<std::string>& args);

// Presenter, once per new game frame.
void NoteGameFrame();

} // namespace Common::PerformanceHint
