/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shad_site.h"
#include "shad_guest.h"

/* Replace only the registration of render-graph pass 61 (volumetricFog).
 * Its constructor supplies a 4x4x4 RGBA16F zero texture at object+1072 and
 * initially publishes that same texture to all three downstream fog inputs.
 * Publish it again before jobs are built; skipping just the GPU dispatch would
 * leave previously rendered fog/history bound by the normal preparation jobs.
 * Allocation, transmittance pass 60 and all other graph registrations remain.
 */
void gow4_disable_volumetric_fog(ShadSiteContext* ctx) {
    const shad_u64 state = *gow4_render_pass_state;
    shad_u64 object = 0;
    if (state && (unsigned int)ctx->gpr->rdi == 61 &&
        (unsigned int)ctx->gpr->rsi == 0xffffffffu && ctx->gpr->rdx == 0)
        object = *(const shad_u64*)(state + 61u * 176u + 168u);

    if (!object || *(const shad_u64*)object != (shad_u64)gow4_fog_vtable) {
        ctx->gpr->rax = gow4_register_pass((int)ctx->gpr->rdi, (int)ctx->gpr->rsi,
                                         ctx->gpr->rdx);
        static unsigned int reported_fallback;
        if (__atomic_exchange_n(&reported_fallback, 1, __ATOMIC_RELAXED) == 0)
            shad_sdk_log(2, 1);
        return;
    }

    const shad_u64 neutral = object + 1072u;
    *gow4_fog_integrated = neutral;
    *gow4_fog_scattering = neutral;
    *gow4_fog_density = neutral;
    /* The call's return is dead: the next instructions load the arguments of
     * pass 64 and do not read RAX. Keep the site's other machine state intact.
     */
    ctx->gpr->rax = 0;
    static unsigned int reported_skip;
    if (__atomic_exchange_n(&reported_skip, 1, __ATOMIC_RELAXED) == 0)
        shad_sdk_log(1, 61);
}
