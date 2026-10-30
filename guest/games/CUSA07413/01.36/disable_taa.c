/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shad_site.h"
#include "shad_guest.h"

/* Exact 01.36 camera setup site: MOV byte ptr [RBX+0x390],1.
 * This store has no register/flag effects. Disabling this site through the
 * patch controller runs the relocated original store again on the next view.
 * The original game still prepares its projection and postprocess outputs.
 * Discovery: TL431 / illusion, PS-Game-Patch GodOfWar-Orbis.xml (Disable TAA).
 */
void gow4_disable_camera_temporal(ShadSiteContext* ctx) {
    *(volatile unsigned char*)(ctx->gpr->rbx + 0x390) = 0;
    static unsigned int observed;
    if (__atomic_fetch_add(&observed, 1, __ATOMIC_RELAXED) == 0)
        shad_sdk_log(1, 0);
}
