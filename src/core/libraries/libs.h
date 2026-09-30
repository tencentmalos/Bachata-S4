// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/arch.h"
#include "core/loader/elf.h"
#include "core/loader/symbols_resolver.h"
#include "core/tls.h"

#if defined(SHADPS4_TYPED_HLE_HOST)
#include "core/guest_cpu/hle/call_adapter.h"
#endif

// One out-of-line registration call per symbol instead of an inlined SymbolResolver per
// LIB_FUNCTION/LIB_OBJ keeps the thousands of registrations small.
void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, char const* nid, char const* lib,
                    u16 libversion, char const* mod, u64 symbol, Core::Loader::SymbolType sym_type);

// On the desktop x86 path the guest and host share the ABI, so the GOT can hold
// the host wrapper pointer directly. On the FEX ARM64 path a translated guest
// cannot call a native pointer: the opt-in host records a typed descriptor for
// future veneer relocation. A descriptor is not production ABI approval; raw
// pointers/callbacks need explicit policies before registration. The host
// pointer is still recorded so debug dumps and any host-side use keep working.
#if defined(SHADPS4_TYPED_HLE_HOST)
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    do {                                                                                           \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Function;                                              \
        auto func = reinterpret_cast<u64>(HOST_CALL(function));                                    \
        sym->AddSymbol(sr, func,                                                                   \
                       Core::GuestCpu::Hle::MakeHleAdapter(function, std::string{nid}));           \
    } while (0)
#else
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(HOST_CALL(function)),     \
                   Core::Loader::SymbolType::Function)
#endif

#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(obj),                     \
                   Core::Loader::SymbolType::Object)

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries
