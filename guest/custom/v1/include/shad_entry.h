/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHAD_ENTRY_V1_H
#define SHAD_ENTRY_V1_H
#include "shad_guest.h"
/* x86-64 SysV + AVX entry observation. Not a guessed function prototype.
 * The callback returns normally, then the adapter restores state and tail-jumps
 * to the original trampoline. Stack arguments and the caller return PC stay put.
 * No exit callback, exception/nonlocal escape, FS/GS change or debugger TF/RF
 * manipulation is supported. Snapshots are valid only during the callback. */
typedef struct ShadEntryGpr {
    shad_u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    shad_u64 r8, r9, r10, r11, r12, r13, r14, r15, rflags;
} ShadEntryGpr;
typedef struct ShadGuestEntryContext {
    shad_u64 rsp;
    const ShadEntryGpr* gpr;
    const unsigned char* fxsave; /* 512 bytes: x87, MXCSR and XMM state. */
    const unsigned char* ymm;    /* 16 registers, 32 bytes each. */
} ShadGuestEntryContext;
#if defined(__cplusplus)
static_assert(sizeof(ShadEntryGpr) == 128 && sizeof(ShadGuestEntryContext) == 32);
#endif
#endif
