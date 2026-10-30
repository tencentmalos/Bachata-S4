/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shad_site.h"
#include "shad_guest.h"

/* Startup site immediately before framebuffer allocation. The displaced
 * MOV EDI,1920 zero-extends RDI and preserves flags. The following MOV ECX,ESI
 * supplies output height. Change render/output geometry together before the
 * game allocates any of these targets; never resize live resources.
 *
 * Based on illusion's Base PS4 720p patch. Unlike wildcard substitutions,
 * this implementation preserves the Pro branch and its mode-switch table.
 */
void gow4_base_720p(ShadSiteContext* ctx) {
    ctx->gpr->rdi = 1920;
    if (*gow4_console_mode == 1 || ctx->gpr->rsi != 1080 || ctx->gpr->rdx != 1920 ||
        *gow4_render_width != 1920 || *gow4_render_height != 1080 ||
        *gow4_output_width != 1920 || *gow4_output_height != 1080) {
        shad_sdk_log(2, *gow4_console_mode);
        return;
    }
    *gow4_render_width = 1280;
    *gow4_render_height = 720;
    *gow4_output_width = 1280;
    *gow4_output_height = 720;
    ctx->gpr->rdi = 1280;
    ctx->gpr->rsi = 720;
    ctx->gpr->rdx = 1280;
    shad_sdk_log(1, 720);
}
