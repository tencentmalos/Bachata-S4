#!/usr/bin/env python3
"""Compile the production tile selector and check foveal spatial precision."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'references/mesa-turnip/src/freedreno/vulkan/tu_util.cc'
text = source.read_text()
begin = text.index('static void\ntu_tiling_config_update_tile_layout(')
end = text.index('\nstatic bool\nis_hw_binning_possible', begin)
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <strings.h>
#define MIN2(a,b) std::min(a,b)
#define MAX2(a,b) std::max(a,b)
#define MIN3(a,b,c) std::min({a,b,c})
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define ROUND_DOWN_TO(a,b) ((a)/(b)*(b))
#define ROUND_DOWN_TO_NPOT(a,b) ROUND_DOWN_TO(a,b)
#define TU_DEBUG(x) false
constexpr unsigned MIN_FDM_TEXEL_SIZE=32;
static unsigned align(unsigned a,unsigned b) { return DIV_ROUND_UP(a,b)*b; }
static unsigned util_align_npot(unsigned a,unsigned b) { return align(a,b); }
static bool util_is_aligned(unsigned a,unsigned b) { return a%b==0; }
struct VkExtent2D { unsigned width,height; };
enum tu_gmem_layout { GMEM };
struct tu_tiling_config {
   VkExtent2D tile0;
   bool possible;
   struct { VkExtent2D tile_count; } vsc;
};
struct gpu_info { unsigned tile_align_h=32, tile_max_w=16416, tile_max_h=16384; };
struct physical { gpu_info *info; };
struct tu_device { physical *physical_device; };
struct tu_framebuffer {
   tu_tiling_config tiling[1];
   unsigned width,height,layers=1,max_tile_w_constraint=~0u,max_tile_h_constraint=~0u;
};
struct tu_render_pass {
   unsigned tile_align_w=96,num_views=1,min_cpp=4,gmem_pixels[1]={4141056};
   bool has_fdm=true;
};
'''
test = r'''
int main() {
   gpu_info info;
   physical physical{&info};
   tu_device device{&physical};
   unsigned checks=0, failures=0;
   auto check=[&](bool ok) { ++checks; if (!ok) { ++failures; printf("FAIL check %u\n",checks); } };
   for (auto extent : {VkExtent2D{2592,2400}, {3376,2976}, {4160,3552}, {8320,3552}})
      for (unsigned layers : {1u,2u})
         for (unsigned budget : {262144u,1048576u,4141056u}) {
            tu_framebuffer fb{{},extent.width,extent.height,layers};
            tu_render_pass pass;
            pass.gmem_pixels[0]=budget;
            tu_tiling_config_update_tile_layout(&fb,&device,&pass,GMEM);
            const auto &t=fb.tiling[0];
            check(t.possible);
            check(t.vsc.tile_count.width>=8 && t.vsc.tile_count.height>=8);
            check(t.tile0.width%96==0 && t.tile0.height%32==0);
            check(uint64_t(t.tile0.width)*t.tile0.height*layers<=budget);
            check(t.tile0.width*t.vsc.tile_count.width>=fb.width &&
                  t.tile0.height*t.vsc.tile_count.height>=fb.height);
         }
   tu_framebuffer fb{{},3376,2976};
   tu_render_pass pass;
   pass.has_fdm=false;
   tu_tiling_config_update_tile_layout(&fb,&device,&pass,GMEM);
   printf("non-FDM tile=%ux%u\n",fb.tiling[0].tile0.width,fb.tiling[0].tile0.height);
   check(fb.tiling[0].tile0.width==1344 && fb.tiling[0].tile0.height==2976);
   printf("production FDM tiling: %u checks / %u failures\n",checks,failures);
   return failures?1:0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(prelude + text[begin:end] + test)
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-Wno-c++11-narrowing',
                    '-fsanitize=undefined', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
