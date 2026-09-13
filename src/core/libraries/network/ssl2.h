// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Ssl2 {

struct OrbisSslData {
    char* ptr;
    u64 size;
};

struct OrbisSslCaCerts {
    OrbisSslData* certs;
    u64 num;
    void* pool;
};

int PS4_SYSV_ABI sceSslInit(std::size_t pool_size);
int PS4_SYSV_ABI sceSslTerm();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Ssl2