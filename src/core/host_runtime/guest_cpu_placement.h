// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

namespace Core::HostRuntime::GuestPlacement {

/// Guest threads (named Guest-N) avoid the lowest-capacity CPUs. A game's job threads run short
/// bursts every frame and the scheduler often leaves them on the little cores, where the frame
/// waits for them. Only with at least four faster CPUs, so a big.LITTLE part with two big cores
/// is left alone.
/// Enable() applies the setting to the live guest threads; ApplyToCurrentThread() to a thread
/// about to run guest code.
void Enable(bool on);
void ApplyToCurrentThread();

/// DebugBus guest_affinity: on | off | status.
std::string Command(const std::vector<std::string>& args);

} // namespace Core::HostRuntime::GuestPlacement
