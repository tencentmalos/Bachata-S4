/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHAD_SITE_V1_H
#define SHAD_SITE_V1_H
#include "shad_entry.h"
/* Mid-function site (sdk_version 2): the handler runs at an instruction boundary
 * with the complete x86-64 SysV + AVX machine state, and may change it.
 *
 *   mode "before":  handler, then the original instructions it displaced.
 *   mode "replace": handler instead of the `expected` instructions; execution
 *                   continues after them. The handler must reproduce every effect
 *                   of those instructions that later code relies on (registers,
 *                   memory, flags).
 *
 * Writes to gpr (except rsp, which is not restorable), rflags and ymm take
 * effect when the handler returns. XMM state is restored from `ymm`, so change
 * vector registers there, not in `fxsave`. The guest red zone below rsp is
 * preserved. The handler runs with the guest MXCSR, so its float math rounds
 * like the replaced instructions. No exceptions, longjmp or FS/GS changes. */
typedef struct ShadSiteContext {
    shad_u64 rsp;          /* guest rsp at the site (read-only) */
    ShadEntryGpr* gpr;     /* rax..r15, rflags */
    unsigned char* fxsave; /* 512 bytes: x87 and MXCSR (XMM taken from ymm) */
    unsigned char* ymm;    /* 16 registers, 32 bytes each */
} ShadSiteContext;
#if defined(__cplusplus)
static_assert(sizeof(ShadSiteContext) == 32);
#endif

/* Scalar lane 0 of xmm/ymm register n, as float. */
static inline float* shad_site_xmm_f32(ShadSiteContext* ctx, unsigned n) {
    return (float*)(ctx->ymm + 32u * n);
}
/* Zero bits 255:128 of ymm register n: what any VEX.128 instruction writing xmm n
 * does to the upper half. A replace handler emulating such an instruction must
 * do the same. */
static inline void shad_site_zero_upper(ShadSiteContext* ctx, unsigned n) {
    __builtin_memset(ctx->ymm + 32u * n + 16u, 0, 16);
}
#endif
