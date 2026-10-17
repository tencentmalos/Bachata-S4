// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/emupsp.h"
#include <SDL3/SDL.h>

namespace EmuPsp {
bool OpenLibrary() {
    return SDL_OpenURL("emupsp://browse?platform=ps4");
}
} // namespace EmuPsp
