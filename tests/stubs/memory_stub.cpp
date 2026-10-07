// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shader recompiler reads SRT tables through the memory manager. The GCN tests map no
// guest memory: every read fails, and the recompiler falls back to its slow path.

#include "core/address_space.h"
#include "core/memory.h"

namespace Core {

struct AddressSpace::Impl {};

AddressSpace::AddressSpace() = default;
AddressSpace::~AddressSpace() = default;

MemoryManager::MemoryManager() = default;
MemoryManager::~MemoryManager() = default;

bool MemoryManager::TryReadSrtMemory(VAddr, void*, u64) {
    return false;
}

} // namespace Core
