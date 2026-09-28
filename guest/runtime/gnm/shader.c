/* SPDX-License-Identifier: GPL-2.0-or-later */
/* App-shipped guest fast path for the Gnm shader-binding encoders.
 *
 * These entries only encode PM4 packets into the caller's command buffer from a
 * small register block; they touch no host state. Running them as guest code
 * removes an HLE crossing per draw (Bloodborne: ~3300 calls/frame, ~25 ms/frame
 * of host HLE wrapper time). Byte-for-byte the output of the host encoders in
 * src/core/libraries/gnmdriver/gnmdriver.cpp, including the checks of the host
 * HLE wrapper (src/core/host_runtime/guest_graphics_hle.cpp): a null command
 * buffer, a zero size or a size above 0x100000 dwords returns -1 before any
 * write. Freestanding x86-64 C (also compiled natively by the tests): no libc,
 * TLS, globals or constructors. Published by GuestRuntime::InstallGnmFastPath.
 *
 * One difference is intended: an unmapped command buffer or register block now
 * faults in the guest, as the console's own library would, where the HLE
 * wrapper returned -1. */

typedef __UINT32_TYPE__ u32;
typedef __INT32_TYPE__ s32;

#define EXPORT __attribute__((visibility("default")))

enum { OpNop = 0x10u, OpSetContextReg = 0x69u, OpSetShReg = 0x76u };

/* One aligned 32-bit store per dword: the command buffer is only dword aligned,
 * and merged 64/128-bit stores would become unaligned (TSO) accesses under FEX. */
static inline void Put(u32* p, int i, u32 v) {
    ((volatile u32*)p)[i] = v;
}
/* PM4 type-3 header: type 3, graphics shader type, no predicate. `body` is the
 * number of dwords after the header. */
static inline u32 Header(u32 op, u32 body) {
    return 0xC0000000u | ((body - 1u) << 16) | (op << 8);
}
/* SET_SH_REG / SET_CONTEXT_REG: register offset, then one to three values. */
static inline u32* Reg1(u32* p, u32 op, u32 reg, u32 a) {
    Put(p, 0, Header(op, 2));
    Put(p, 1, reg);
    Put(p, 2, a);
    return p + 3;
}
static inline u32* Reg2(u32* p, u32 op, u32 reg, u32 a, u32 b) {
    Put(p, 0, Header(op, 3));
    Put(p, 1, reg);
    Put(p, 2, a);
    Put(p, 3, b);
    return p + 4;
}
static inline u32* Reg3(u32* p, u32 op, u32 reg, u32 a, u32 b, u32 c) {
    Put(p, 0, Header(op, 4));
    Put(p, 1, reg);
    Put(p, 2, a);
    Put(p, 3, b);
    Put(p, 4, c);
    return p + 5;
}
/* NOP packets carrying a wrapped register update: two or three body dwords. */
static inline u32* Nop2(u32* p, u32 a, u32 b) {
    Put(p, 0, Header(OpNop, 2));
    Put(p, 1, a);
    Put(p, 2, b);
    return p + 3;
}
static inline u32* Nop3(u32* p, u32 a, u32 b, u32 c) {
    Put(p, 0, Header(OpNop, 3));
    Put(p, 1, a);
    Put(p, 2, b);
    Put(p, 3, c);
    return p + 4;
}
/* Ending NOP with an N-dword data block; only its first data dword is written. */
static inline void TrailingNop(u32* p, u32 n) {
    Put(p, 0, Header(OpNop, n));
    Put(p, 1, 0);
}
/* Host HLE wrapper checks, applied before the encoder's own. */
static inline int Admitted(const u32* cmdbuf, u32 size) {
    return cmdbuf && size && size <= 0x100000u;
}

#define SH OpSetShReg
#define CTX OpSetContextReg

EXPORT s32 shad_sceGnmSetCsShader(u32* p, u32 size, const u32* cs) {
    if (!Admitted(p, size) || size <= 0x18 || !cs || cs[1] != 0)
        return -1;
    p = Reg2(p, SH, 0x20cu, cs[0], 0u);          /* COMPUTE_PGM_LO/HI */
    p = Reg2(p, SH, 0x212u, cs[2], cs[3]);       /* COMPUTE_PGM_RSRC1/RSRC2 */
    p = Reg3(p, SH, 0x207u, cs[4], cs[5], cs[6]); /* COMPUTE_NUM_THREAD_X/Y/Z */
    TrailingNop(p, 11);
    return 0;
}

EXPORT s32 shad_sceGnmSetCsShaderWithModifier(u32* p, u32 size, const u32* cs, u32 modifier) {
    if (!Admitted(p, size) || size <= 0x18 || !cs || (modifier & 0xfffffc3fu) != 0 || cs[1] != 0)
        return -1;
    const u32 rsrc1 = modifier == 0 ? cs[2] : (cs[2] & 0xfffffc3fu) | modifier;
    p = Reg2(p, SH, 0x20cu, cs[0], 0u);
    p = Reg2(p, SH, 0x212u, rsrc1, cs[3]);
    p = Reg3(p, SH, 0x207u, cs[4], cs[5], cs[6]);
    TrailingNop(p, 11);
    return 0;
}

EXPORT s32 shad_sceGnmSetEsShader(u32* p, u32 size, const u32* es, u32 modifier) {
    if (!Admitted(p, size) || size < 0x14 || !es || (modifier & 0xfcfffc3fu) || es[1] != 0)
        return -1;
    const u32 rsrc1 = modifier == 0 ? es[2] : (es[2] & 0xfcfffc3fu) | modifier;
    p = Reg2(p, SH, 0xc8u, es[0], 0u);    /* SPI_SHADER_PGM_LO_ES */
    p = Reg2(p, SH, 0xcau, rsrc1, es[3]); /* SPI_SHADER_PGM_RSRC1_ES */
    TrailingNop(p, 11);
    return 0;
}

EXPORT s32 shad_sceGnmSetGsShader(u32* p, u32 size, const u32* gs) {
    if (!Admitted(p, size) || size < 0x1d || !gs || gs[1] != 0)
        return -1;
    p = Reg2(p, SH, 0x88u, gs[0], 0u);
    p = Reg2(p, SH, 0x8au, gs[2], gs[3]);
    p = Reg1(p, CTX, 0x2e5u, gs[4]); /* VGT_STRMOUT_CONFIG */
    p = Reg1(p, CTX, 0x29bu, gs[5]); /* VGT_GS_OUT_PRIM_TYPE */
    p = Reg1(p, CTX, 0x2e4u, gs[6]); /* VGT_GS_INSTANCE_CNT */
    TrailingNop(p, 11);
    return 0;
}

EXPORT s32 shad_sceGnmSetHsShader(u32* p, u32 size, const u32* hs, u32 ls_hs_config) {
    if (!Admitted(p, size) || size < 0x1e || !hs || hs[1] != 0)
        return -1;
    p = Reg2(p, SH, 0x108u, hs[0], 0u);
    p = Reg2(p, SH, 0x10au, hs[2], hs[3]);
    p = Reg2(p, CTX, 0x286u, hs[5], hs[6]);  /* VGT_HOS_MAX/MIN_TESS_LEVEL */
    p = Reg1(p, CTX, 0x2dbu, hs[4]);         /* VGT_TF_PARAM */
    p = Reg1(p, CTX, 0x2d6u, ls_hs_config);  /* VGT_LS_HS_CONFIG */
    TrailingNop(p, 11);
    return 0;
}

EXPORT s32 shad_sceGnmSetLsShader(u32* p, u32 size, const u32* ls, u32 modifier) {
    if (!Admitted(p, size) || size < 0x17 || !ls)
        return -1;
    const u32 mask = (modifier & 0xfffffc3fu) == 0 ? 0xfffffc3fu : 0xfcfffc3fu;
    if ((modifier & mask) || ls[1] != 0)
        return -1;
    const u32 rsrc1 = modifier == 0 ? ls[2] : (ls[2] & mask) | modifier;
    p = Reg2(p, SH, 0x148u, ls[0], 0u);    /* SPI_SHADER_PGM_LO_LS */
    p = Reg1(p, SH, 0x14bu, ls[3]);        /* SPI_SHADER_PGM_RSRC2_LS */
    p = Reg2(p, SH, 0x14au, rsrc1, ls[3]); /* SPI_SHADER_PGM_RSRC1_LS */
    TrailingNop(p, 11);
    return 0;
}

/* The PS register writes shared by SetPsShader and SetPsShader350. */
static void PsRegisters(u32* p, const u32* ps) {
    p = Reg2(p, SH, 8u, ps[0], 0u);          /* SPI_SHADER_PGM_LO/HI_PS */
    p = Reg2(p, SH, 10u, ps[2], ps[3]);      /* SPI_SHADER_PGM_RSRC1/RSRC2_PS */
    p = Reg2(p, CTX, 0x1c4u, ps[4], ps[5]);  /* SPI_SHADER_Z_FORMAT/COL_FORMAT */
    p = Reg2(p, CTX, 0x1b3u, ps[6], ps[7]);  /* SPI_PS_INPUT_ENA/ADDR */
    p = Reg1(p, CTX, 0x1b6u, ps[8]);         /* SPI_PS_IN_CONTROL */
    p = Reg1(p, CTX, 0x1b8u, ps[9]);         /* SPI_BARYC_CNTL */
    p = Reg1(p, CTX, 0x203u, ps[10]);        /* DB_SHADER_CONTROL */
    p = Reg1(p, CTX, 0x8fu, ps[11]);         /* CB_SHADER_MASK */
    TrailingNop(p, 11);
}

EXPORT s32 shad_sceGnmSetPsShader(u32* p, u32 size, const u32* ps) {
    if (!Admitted(p, size) || size <= 0x27)
        return -1;
    if (!ps) {
        p = Reg2(p, SH, 8u, 0u, 0u);
        p = Reg1(p, CTX, 0x203u, 0u);
        TrailingNop(p, 0x20);
        return 0;
    }
    if (ps[1] != 0)
        return -1;
    PsRegisters(p, ps);
    return 0;
}

EXPORT s32 shad_sceGnmSetPsShader350(u32* p, u32 size, const u32* ps) {
    if (!Admitted(p, size) || size <= 0x27)
        return -1;
    if (!ps) {
        p = Reg2(p, SH, 8u, 0u, 0u);
        p = Reg1(p, CTX, 0x203u, 0u);
        p = Reg1(p, CTX, 0x8fu, 0xfu);
        TrailingNop(p, 0x1d);
        return 0;
    }
    if (ps[1] != 0)
        return -1;
    PsRegisters(p, ps);
    return 0;
}

EXPORT s32 shad_sceGnmSetVsShader(u32* p, u32 size, const u32* vs, u32 modifier) {
    if (!Admitted(p, size) || size <= 0x1c || !vs || (modifier & 0xfcfffc3fu) || vs[1] != 0)
        return -1;
    const u32 rsrc1 = modifier == 0 ? vs[2] : (vs[2] & 0xfcfffc3fu) | modifier;
    p = Reg2(p, SH, 0x48u, vs[0], 0u);    /* SPI_SHADER_PGM_LO_VS */
    p = Reg2(p, SH, 0x4au, rsrc1, vs[3]); /* SPI_SHADER_PGM_RSRC1_VS */
    p = Reg1(p, CTX, 0x207u, vs[6]);      /* PA_CL_VS_OUT_CNTL */
    p = Reg1(p, CTX, 0x1b1u, vs[4]);      /* SPI_VS_OUT_CONFIG */
    p = Reg1(p, CTX, 0x1c3u, vs[5]);      /* SPI_SHADER_POS_FORMAT */
    TrailingNop(p, 11);
    return 0;
}

/* The Update* variants write the context registers as NOP-wrapped
 * SET_CONTEXT_REG packets (0xc01e.... is the inner packet header). */
EXPORT s32 shad_sceGnmUpdateGsShader(u32* p, u32 size, const u32* gs) {
    if (!Admitted(p, size) || size < 0x1d || !gs || gs[1] != 0)
        return -1;
    p = Reg2(p, SH, 0x88u, gs[0], 0u);
    p = Reg2(p, SH, 0x8au, gs[2], gs[3]);
    p = Nop2(p, 0xc01e02e5u, gs[4]);
    p = Nop2(p, 0xc01e029bu, gs[5]);
    p = Nop2(p, 0xc01e02e4u, gs[6]);
    TrailingNop(p, 11);
    return 0;
}

EXPORT s32 shad_sceGnmUpdateHsShader(u32* p, u32 size, const u32* hs, u32 ls_hs_config) {
    if (!Admitted(p, size) || size <= 0x1c || !hs || hs[1] != 0)
        return -1;
    p = Reg2(p, SH, 0x108u, hs[0], 0u);
    p = Reg2(p, SH, 0x10au, hs[2], hs[3]);
    p = Nop3(p, 0xc01e0286u, hs[5], hs[6]);
    p = Nop2(p, 0xc01e02dbu, hs[4]);
    p = Nop2(p, 0xc01e02d6u, ls_hs_config);
    TrailingNop(p, 11);
    return 0;
}

static void UpdatePsRegisters(u32* p, const u32* ps) {
    p = Reg2(p, SH, 8u, ps[0], 0u);
    p = Reg2(p, SH, 10u, ps[2], ps[3]);
    p = Nop3(p, 0xc01e01c4u, ps[4], ps[5]);
    p = Nop3(p, 0xc01e01b3u, ps[6], ps[7]);
    p = Nop2(p, 0xc01e01b6u, ps[8]);
    p = Nop2(p, 0xc01e01b8u, ps[9]);
    p = Nop2(p, 0xc01e0203u, ps[10]);
    p = Nop2(p, 0xc01e008fu, ps[11]);
    TrailingNop(p, 11);
}

EXPORT s32 shad_sceGnmUpdatePsShader(u32* p, u32 size, const u32* ps) {
    if (!Admitted(p, size) || size <= 0x27)
        return -1;
    if (!ps) {
        p = Reg2(p, SH, 8u, 0u, 0u);
        p = Nop2(p, 0xc01e0203u, 0u);
        TrailingNop(p, 0x20);
        return 0;
    }
    if (ps[1] != 0)
        return -1;
    UpdatePsRegisters(p, ps);
    return 0;
}

EXPORT s32 shad_sceGnmUpdatePsShader350(u32* p, u32 size, const u32* ps) {
    if (!Admitted(p, size) || size <= 0x27)
        return -1;
    if (!ps) {
        p = Reg2(p, SH, 8u, 0u, 0u);
        p = Nop2(p, 0xc01e0203u, 0u);
        p = Reg1(p, CTX, 0x8fu, 0xfu);
        TrailingNop(p, 0x1d);
        return 0;
    }
    if (ps[1] != 0)
        return -1;
    UpdatePsRegisters(p, ps);
    return 0;
}

EXPORT s32 shad_sceGnmUpdateVsShader(u32* p, u32 size, const u32* vs, u32 modifier) {
    if (!Admitted(p, size) || size <= 0x1c || !vs || (modifier & 0xfcfffc3fu) || vs[1] != 0)
        return -1;
    const u32 rsrc1 = modifier == 0 ? vs[2] : (vs[2] & 0xfcfffc3fu) | modifier;
    p = Reg2(p, SH, 0x48u, vs[0], 0u);
    p = Reg2(p, SH, 0x4au, rsrc1, vs[3]);
    p = Nop2(p, 0xc01e0207u, vs[6]);
    p = Nop2(p, 0xc01e01b1u, vs[4]);
    p = Nop2(p, 0xc01e01c3u, vs[5]);
    TrailingNop(p, 11);
    return 0;
}

/* Retail firmware: user PA is never enabled. */
EXPORT u32 shad_sceGnmIsUserPaEnabled(void) {
    return 0;
}
