// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace GpuReshape {
// Published on the renderer thread. Readers never call the SDK or wait on GPU/teardown.
std::string StatusSnapshot();
}
