// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/libraries/kernel/sync/lock_word.h"

namespace Libraries::Kernel {

/// The pthread mutex word: the shared three-state lock word (sync/lock_word.h), the same
/// protocol as the Android guest mutex and the app-shipped guest fast path.
using TimedMutex = Sync::LockWord;

} // namespace Libraries::Kernel
