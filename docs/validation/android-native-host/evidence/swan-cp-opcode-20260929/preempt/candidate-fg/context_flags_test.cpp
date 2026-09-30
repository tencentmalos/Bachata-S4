#include <initializer_list>

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
#define KGSL_CONTEXT_SAVE_GMEM		0x00000001
#define KGSL_CONTEXT_NO_GMEM_ALLOC	0x00000002
#define KGSL_CONTEXT_PREAMBLE		0x00000010
#define KGSL_CONTEXT_PREEMPT_STYLE_RINGBUFFER 0x1
#define KGSL_CONTEXT_PREEMPT_STYLE_FINEGRAIN  0x2
#define KGSL_CONTEXT_PREEMPT_STYLE_DEFAULT    0x0
#define KGSL_CONTEXT_PREEMPT_STYLE_MASK       0x0E000000
#define KGSL_CONTEXT_PREEMPT_STYLE_SHIFT      25
static int
kgsl_submitqueue_new(struct tu_device *dev, struct tu_queue *queue)
{
   struct kgsl_drawctxt_create req = {
      .flags = KGSL_CONTEXT_SAVE_GMEM |
              KGSL_CONTEXT_NO_GMEM_ALLOC |
              KGSL_CONTEXT_PREAMBLE,
   };

   const bool preempt_rb = TU_DEBUG_START(KGSL_PREEMPT_RB);
   const bool preempt_fg = TU_DEBUG_START(KGSL_PREEMPT_FG);
   if (preempt_rb && preempt_fg) {
      mesa_loge("KGSL preemption: kgsl_preempt_rb and kgsl_preempt_fg "
                "cannot be enabled together");
      errno = EINVAL;
      return -1;
   }
   const uint32_t preempt_style =
      preempt_rb ? KGSL_CONTEXT_PREEMPT_STYLE_RINGBUFFER :
      preempt_fg ? KGSL_CONTEXT_PREEMPT_STYLE_FINEGRAIN :
                   KGSL_CONTEXT_PREEMPT_STYLE_DEFAULT;
   req.flags |= preempt_style << KGSL_CONTEXT_PREEMPT_STYLE_SHIFT;
   const uint32_t requested_flags = req.flags;

   int ret = safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
   if (preempt_rb || preempt_fg) {
      const int saved_errno = errno;
      mesa_logi("KGSL preemption: requested_flags=0x%x returned_flags=0x%x "
                "requested_style=%u returned_style=%u context=%u "
                "result=%d errno=%d",
                requested_flags, req.flags, preempt_style,
                (req.flags & KGSL_CONTEXT_PREEMPT_STYLE_MASK) >>
                   KGSL_CONTEXT_PREEMPT_STYLE_SHIFT,
                ret ? 0 : req.drawctxt_id, ret,
                ret ? saved_errno : 0);
      errno = saved_errno;
   }
   if (ret)
      return ret;

   queue->msm_queue_id = req.drawctxt_id;

   return 0;
}

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
