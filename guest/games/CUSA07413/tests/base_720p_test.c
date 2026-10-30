/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "shad_site.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned int mode, rw, rh, ow, oh;
static const unsigned int *const gow4_console_mode = &mode;
static unsigned int *const gow4_render_width = &rw, *const gow4_render_height =
                                                        &rh;
static unsigned int *const gow4_output_width = &ow, *const gow4_output_height =
                                                        &oh;
static shad_u64 tag;
static shad_i64 value;
shad_u64 shad_sdk_log(shad_u64 t, shad_i64 v) {
  tag = t;
  value = v;
  return 0;
}
#include "../01.36/base_720p.c"
int main(void) {
  for (unsigned int variant = 0; variant < 9; ++variant) {
    ShadEntryGpr g, expect;
    unsigned char ymm[512], fx[512], beforeY[512], beforeF[512];
    memset(&g, 0xa5, sizeof g);
    memset(ymm, 0x53, 512);
    memset(fx, 0x71, 512);
    memcpy(beforeY, ymm, 512);
    memcpy(beforeF, fx, 512);
    mode = 0;
    rw = ow = 1920;
    rh = oh = 1080;
    g.rsi = 1080;
    g.rdx = 1920;
    if (variant == 1)
      mode = 1;
    if (variant == 2)
      g.rsi = 2160;
    if (variant == 3)
      g.rdx = 3840;
    if (variant == 4)
      rw = 1919;
    if (variant == 5)
      rh = 1079;
    if (variant == 6)
      ow = 1919;
    if (variant == 7)
      oh = 1079;
    if (variant == 8) {
      mode = 1;
      g.rsi = rh = oh = 2160;
      g.rdx = ow = 3840;
    }
    unsigned int dims[] = {rw, rh, ow, oh};
    expect = g;
    expect.rdi = 1920;
    if (!variant) {
      expect.rdi = 1280;
      expect.rsi = 720;
      expect.rdx = 1280;
    }
    ShadSiteContext ctx = {
        .rsp = 0x123400, .gpr = &g, .fxsave = fx, .ymm = ymm};
    gow4_base_720p(&ctx);
    assert(!memcmp(&g, &expect, sizeof g));
    assert(!memcmp(ymm, beforeY, 512));
    assert(!memcmp(fx, beforeF, 512));
    assert(ctx.rsp == 0x123400);
    if (!variant) {
      assert(rw == 1280 && rh == 720 && ow == 1280 && oh == 720 && tag == 1 &&
             value == 720);
    } else {
      assert(rw == dims[0] && rh == dims[1] && ow == dims[2] && oh == dims[3] &&
             tag == 2);
    }
  }
  puts("GOW4_720P_SITE_PASS 9 cases: base, Pro, arguments, globals, "
       "register/flags/vector preservation");
}
