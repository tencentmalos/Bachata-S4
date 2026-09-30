from pathlib import Path
import subprocess,hashlib,json
root=Path.cwd();out=root/'build/validation/swan-cp-live-20260929/candidate'
def func(path,name):
 s=Path(path).read_text();a=s.index(name+'(');a=s.rfind('\n',0,a)+1;b=s.index('{',a);n=1;i=b+1
 while n:
  n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
a=func('references/mesa-turnip/src/freedreno/vulkan/tu_cs.cc','tu_cs_align_draw_state')
e=func('references/mesa-turnip/src/freedreno/vulkan/tu_cs.h','tu_cs_emit_pkt7')
prefix=r'''
#include <cassert>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "freedreno_pm4.h"
#define VK_SUCCESS 0
#define TU_CS_MODE_GROW 0
#define TU_CS_MODE_EXTERNAL 1
#define TU_BREADCRUMBS_ENABLED 0
static bool enabled;
#define TU_DEBUG_START(x) enabled
struct tu_cs {uint32_t *start,*cur,*end,*reserved_end; int mode,status; bool pkt; uint64_t iova; std::vector<uint32_t> replacement;};
static void mesa_logi(const char*, ...) {}
static uint64_t tu_cs_get_cur_iova(const tu_cs* c) {return c->iova+4*(c->cur-c->start);}
static void tu_cs_reserve(tu_cs* c,unsigned size) {
 if(c->end-c->cur<int(size)) {assert(c->mode==0); c->replacement.assign(4096,0xfacefeed);c->start=c->cur=c->replacement.data();c->end=c->start+4096;c->iova+=0x20000;}
 c->reserved_end=c->cur+size;assert(c->reserved_end<=c->end);
}
static void tu_cs_emit(tu_cs* c,uint32_t x){assert(c->cur<c->reserved_end);*c->cur++=x;}
'''
main=r'''
int main(){uint64_t tests=0,crossing_control=0,padded=0,rollovers=0;
 for(int flag=0;flag<2;flag++) for(int external=0;external<2;external++) for(int roll=0;roll<2;roll++) {
  if(external&&roll)continue;
  for(unsigned offset=0;offset<1024;offset++) for(unsigned cnt=3;cnt<=96;cnt+=3){
   enabled=flag;std::vector<uint32_t> mem(4096,0xfacefeed);tu_cs c{mem.data(),mem.data()+offset,mem.data()+(roll?offset+cnt:4096),mem.data()+offset,external,0,false,0x41bf626000,{}};
   tu_cs_emit_pkt7(&c,CP_SET_DRAW_STATE,cnt);for(unsigned j=0;j<cnt;j++)tu_cs_emit(&c,0x11000000+j);
   const auto *p=c.replacement.empty()?mem.data()+offset:c.replacement.data();const auto *first=p;
   if(cp_type7_opcode(*p)==CP_NOP){assert(pkt_is_type7(*p));unsigned n=type7_pkt_size(*p);for(unsigned j=1;j<=n;j++)assert(p[j]==0);p+=n+1;padded++;}
   assert(pkt_is_type7(*p));assert(cp_type7_opcode(*p)==CP_SET_DRAW_STATE);assert(type7_pkt_size(*p)==cnt);
   uint64_t addr=c.iova+4*(p-c.start);bool crosses=(addr>>12)!=((addr+4*cnt)>>12);
   if(enabled&&!external)assert(!crosses);else if(crosses)crossing_control++;
   for(unsigned j=0;j<cnt;j++)assert(p[1+j]==0x11000000+j);
   assert(p+1+cnt==c.cur);assert(*c.cur==0xfacefeed);
   for(unsigned j=0;j<offset;j++)assert(mem[j]==0xfacefeed);
   if(!c.replacement.empty())rollovers++;
   tests++;
  }
 }
 assert(crossing_control>0&&padded>0&&rollovers>0);
 printf("cases=%" PRIu64 " padded=%" PRIu64 " rollover=%" PRIu64 " crossing_controls=%" PRIu64 " failures=0\n",tests,padded,rollovers,crossing_control);
}
'''
cpp=out/'page_align_test.cpp';cpp.write_text(prefix+'\nvoid\n'+a+'\nvoid\n'+e+main)
cmd=['clang++','-std=c++17','-O1','-g','-fsanitize=undefined','-I'+str(root/'references/mesa-turnip/src/freedreno/common'),'-I'+str(root/'build/turnip-api33/native/src/freedreno/registers/adreno'),str(cpp),'-o',str(out/'page_align_test_ubsan')]
subprocess.run(cmd,check=True);r=subprocess.check_output([str(out/'page_align_test_ubsan')],text=True,timeout=30);print(r);(out/'page-align-test.txt').write_text(r);(out/'tested-functions.sha256').write_text(hashlib.sha256((a+e).encode()).hexdigest()+'\n')
