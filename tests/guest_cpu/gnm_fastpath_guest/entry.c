/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test driver linked together with the production guest/runtime/gnm payload.
 * It calls the payload's exports the way a game import would, one case each,
 * so the test can compare every result with the natively compiled source. */
typedef __UINT64_TYPE__ u64;
typedef __UINT32_TYPE__ u32;
typedef __INT32_TYPE__ s32;

s32 shad_sceGnmSetCsShader(u32*, u32, const u32*);
s32 shad_sceGnmSetCsShaderWithModifier(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetEsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetGsShader(u32*, u32, const u32*);
s32 shad_sceGnmSetHsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetLsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmSetPsShader(u32*, u32, const u32*);
s32 shad_sceGnmSetPsShader350(u32*, u32, const u32*);
s32 shad_sceGnmSetVsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmUpdateGsShader(u32*, u32, const u32*);
s32 shad_sceGnmUpdateHsShader(u32*, u32, const u32*, u32);
s32 shad_sceGnmUpdatePsShader(u32*, u32, const u32*);
s32 shad_sceGnmUpdatePsShader350(u32*, u32, const u32*);
s32 shad_sceGnmUpdateVsShader(u32*, u32, const u32*, u32);
u32 shad_sceGnmIsUserPaEnabled(void);

/* Same layout as tests/guest_cpu/gnm_fastpath_guest_tests.cpp. */
struct Case {
    u64 fn, cmdbuf, size, regs, extra, result;
};
struct Batch {
    u64 count, repeat;
    struct Case* cases;
};

static u64 Run(const struct Case* c) {
    u32* p = (u32*)c->cmdbuf;
    const u32 size = (u32)c->size;
    const u32* r = (const u32*)c->regs;
    const u32 x = (u32)c->extra;
    switch (c->fn) {
    case 0: return (u32)shad_sceGnmSetCsShader(p, size, r);
    case 1: return (u32)shad_sceGnmSetCsShaderWithModifier(p, size, r, x);
    case 2: return (u32)shad_sceGnmSetEsShader(p, size, r, x);
    case 3: return (u32)shad_sceGnmSetGsShader(p, size, r);
    case 4: return (u32)shad_sceGnmSetHsShader(p, size, r, x);
    case 5: return (u32)shad_sceGnmSetLsShader(p, size, r, x);
    case 6: return (u32)shad_sceGnmSetPsShader(p, size, r);
    case 7: return (u32)shad_sceGnmSetPsShader350(p, size, r);
    case 8: return (u32)shad_sceGnmSetVsShader(p, size, r, x);
    case 9: return (u32)shad_sceGnmUpdateGsShader(p, size, r);
    case 10: return (u32)shad_sceGnmUpdateHsShader(p, size, r, x);
    case 11: return (u32)shad_sceGnmUpdatePsShader(p, size, r);
    case 12: return (u32)shad_sceGnmUpdatePsShader350(p, size, r);
    case 13: return (u32)shad_sceGnmUpdateVsShader(p, size, r, x);
    case 14: return shad_sceGnmIsUserPaEnabled();
    default: return 0xbadu;
    }
}

__attribute__((visibility("default"))) u64 guest_entry(struct Batch* b) {
    for (u64 k = 0; k < b->repeat; ++k)
        for (u64 i = 0; i < b->count; ++i)
            b->cases[i].result = Run(&b->cases[i]);
    return b->count;
}
