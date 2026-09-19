// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shad_guest.h"
using TmntU32 = __UINT32_TYPE__;
// Offsets are independently checked against instructions, not decompiler names.
// Integer guest addresses avoid undefined C++ subtraction of unrelated pointers.
struct TmntCommandBuffer {
    shad_u64 begin;                         // +0x00
    shad_u64 end;                           // +0x08
    shad_u64 cursor;                        // +0x10
    shad_u64 (*grow)(TmntCommandBuffer*, TmntU32, shad_u64); // +0x18; AL is success
    shad_u64 grow_context;                  // +0x20
};
static_assert(sizeof(TmntCommandBuffer) == 0x28);
static_assert(__builtin_offsetof(TmntCommandBuffer, cursor) == 0x10);
static_assert(__builtin_offsetof(TmntCommandBuffer, grow) == 0x18);
struct TmntSubmitContext {
    unsigned char storage[0x1c8];
};
static_assert(sizeof(TmntSubmitContext) == 0x1c8);
