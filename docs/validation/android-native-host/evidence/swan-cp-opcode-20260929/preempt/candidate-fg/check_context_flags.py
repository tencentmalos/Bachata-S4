from pathlib import Path
import re,subprocess,json,hashlib
root=Path.cwd();out=Path(__file__).resolve().parent;source=root/'references/mesa-turnip/src/freedreno/vulkan/tu_knl_kgsl.cc';s=source.read_text();start=s.index('static int\nkgsl_submitqueue_new(');end=s.index('\nstatic void\nkgsl_submitqueue_close',start);fn=s[start:end]
uapi=(root/'references/mesa-turnip/src/freedreno/vulkan/msm_kgsl.h').read_text();names=['SAVE_GMEM','NO_GMEM_ALLOC','PREAMBLE','PREEMPT_STYLE_RINGBUFFER','PREEMPT_STYLE_FINEGRAIN','PREEMPT_STYLE_DEFAULT','PREEMPT_STYLE_MASK','PREEMPT_STYLE_SHIFT'];defs='\n'.join(re.search(r'^#define KGSL_CONTEXT_'+name+r'\s+[^\n]+',uapi,re.M)[0] for name in names)
preamble=r'''
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdarg>
struct physical_device { int local_fd; };
struct tu_device { physical_device *physical_device; };
struct tu_queue { uint32_t msm_queue_id; };
struct kgsl_drawctxt_create { uint32_t flags, drawctxt_id; };
static bool rb,fg; static int calls,logs,io_result,io_errno; static uint32_t observed,returned;
#define TU_DEBUG_START(x) test_##x()
bool test_KGSL_PREEMPT_RB(){return rb;}
bool test_KGSL_PREEMPT_FG(){return fg;}
#define IOCTL_KGSL_DRAWCTXT_CREATE 42
int safe_ioctl(int fd, unsigned long request, void *data){calls++; auto *req=(kgsl_drawctxt_create*)data; observed=req->flags;req->flags=returned;req->drawctxt_id=51;errno=io_errno;return io_result;}
void mesa_logi(const char *,...){logs++;errno=ECHILD;}
void mesa_loge(const char *,...){logs++;errno=ECHILD;}
'''
main=r'''
int checks=0, failures=0;
void check(bool ok,const char *s){checks++;if(!ok){failures++;std::printf("FAIL %s\n",s);}}
int main(){
 physical_device phy{4};tu_device dev{&phy};
 for(unsigned mode=0;mode<3;mode++)for(bool fail:{false,true}){
  rb=mode==1;fg=mode==2;calls=logs=0;io_result=fail?-1:0;io_errno=fail?EIO:0;returned=0x8052u|(mode<<25);tu_queue q{0xdead};
  int result=kgsl_submitqueue_new(&dev,&q);
  check(calls==1,"one ioctl");check(observed==(0x13u|(mode<<25)),"exact request bits");check(result==io_result,"return value");check(q.msm_queue_id==(fail?0xdead:51),"queue identity on success only");check(logs==(mode?1:0),"logging gate");check(errno==io_errno,"preserve ioctl errno");
 }
 rb=fg=true;calls=logs=0;tu_queue q{0xdead};
 check(kgsl_submitqueue_new(&dev,&q)==-1,"conflicting flags rejected");check(errno==EINVAL,"conflict errno");check(calls==0,"no ioctl for conflict");check(q.msm_queue_id==0xdead,"conflict leaves queue");
 std::printf("%d checks / %d failures\n",checks,failures);return failures?1:0;
}
'''.replace('#include','')
cpp=out/'context_flags_test.cpp';cpp.write_text('#include <initializer_list>\n'+preamble+defs+'\n'+fn+main);binary=out/'context_flags_test'
subprocess.run(['clang++','-std=c++17','-fsanitize=undefined','-fno-sanitize-recover=all','-Werror',str(cpp),'-o',str(binary)],check=True)
r=subprocess.run([str(binary)],capture_output=True,text=True,check=True);(out/'context-flags-test.txt').write_text(r.stdout);print(r.stdout,end='')
(out/'context-flags-test.json').write_text(json.dumps({'production_source':str(source.relative_to(root)),'production_function_sha256':hashlib.sha256(fn.encode()).hexdigest(),'result':r.stdout.strip(),'scope':'Host test extracts production function and real UAPI constants; mocked ioctl. Does not validate KMD/GMU behavior or GPU stability.'},indent=2))
