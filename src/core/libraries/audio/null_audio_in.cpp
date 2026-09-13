// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audioin_backend.h"

namespace Libraries::AudioIn {

std::unique_ptr<PortInBackend> NullAudioIn::Open(PortIn&) {
    // sceAudioInOpen returns NOT_OPENED and publishes no port. Actual Android
    // recording is not implemented; do not fabricate samples or availability.
    return nullptr;
}

} // namespace Libraries::AudioIn
