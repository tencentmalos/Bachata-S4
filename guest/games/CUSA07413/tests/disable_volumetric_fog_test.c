/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shad_site.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned long long states[1364], object[158], vtable[9];
static unsigned long long state_address, integrated, scattering, density;
static const unsigned long long* const gow4_render_pass_state = &state_address;
static const unsigned long long* const gow4_fog_vtable = vtable;
static unsigned long long* const gow4_fog_integrated = &integrated;
static unsigned long long* const gow4_fog_scattering = &scattering;
static unsigned long long* const gow4_fog_density = &density;
static unsigned calls;
static int called_id, called_view;
static unsigned long long called_owner;

unsigned long long gow4_register_pass(int id, int view, unsigned long long owner) {
    ++calls;
    called_id = id;
    called_view = view;
    called_owner = owner;
    return 0x12345678;
}

shad_u64 shad_sdk_log(shad_u64 tag, shad_i64 value) {
    assert((tag == 1 && value == 61) || (tag == 2 && value == 1));
    return 0;
}

#include "../01.36/disable_volumetric_fog.c"

int main(void) {
    for (unsigned variant = 0; variant < 7; ++variant) {
        ShadEntryGpr g, expected;
        unsigned char ymm[512], fx[512], before_y[512], before_f[512];
        memset(&g, 0xa5, sizeof g);
        memset(ymm, 0x39, sizeof ymm);
        memset(fx, 0x57, sizeof fx);
        memcpy(before_y, ymm, sizeof ymm);
        memcpy(before_f, fx, sizeof fx);
        state_address = (unsigned long long)states;
        states[1363] = (unsigned long long)object;
        object[0] = (unsigned long long)vtable;
        integrated = 0x1111;
        scattering = 0x2222;
        density = 0x3333;
        calls = 0;
        g.rdi = 61;
        g.rsi = 0xffffffff;
        g.rdx = 0;
        if (variant == 1)
            g.rdi = 60;
        if (variant == 2)
            state_address = 0;
        if (variant == 3)
            states[1363] = 0;
        if (variant == 4)
            object[0] = 0;
        if (variant == 5)
            g.rsi = 1;
        if (variant == 6)
            g.rdx = 0x9876;
        expected = g;
        expected.rax = variant ? 0x12345678 : 0;
        ShadSiteContext ctx = {.rsp = 0x123400, .gpr = &g, .fxsave = fx, .ymm = ymm};
        gow4_disable_volumetric_fog(&ctx);
        assert(!memcmp(&g, &expected, sizeof g));
        assert(!memcmp(ymm, before_y, sizeof ymm));
        assert(!memcmp(fx, before_f, sizeof fx));
        assert(ctx.rsp == 0x123400);
        if (!variant) {
            assert(calls == 0);
            assert(integrated == (unsigned long long)object + 1072);
            assert(scattering == integrated && density == integrated);
        } else {
            assert(calls == 1 && called_id == (int)g.rdi && called_view == (int)g.rsi &&
                   called_owner == g.rdx);
            assert(integrated == 0x1111 && scattering == 0x2222 && density == 0x3333);
        }
    }
    puts("GOW4_FOG_SITE_PASS 7 cases: neutral output, fallbacks, unchanged machine state");
}
