// SPDX-License-Identifier: GPL-2.0-or-later
//
// kgsl_slot_repro: minimal reproduction of the Adreno A8XX "CP reads zeros" fault
// (CP DDE BR / CP BV opcode error with opcode 0) without the emulator, Mesa or
// Vulkan. It talks to /dev/kgsl-3d0 directly and runs as the plain adb shell user.
//
// Layout under test, found on GoW3 and KH3 (docs/validation/android-native-host/
// a8xx-dde-opcode-zero-20261017.md, section 9): a GPU read-only buffer whose first
// page sits in 4 KiB slot 6 of a 64 KiB GPU VA block, while the lower slots of the
// same block belonged to a buffer that has been freed (unmapped, TLB invalidated).
// The CP then reads slot 7 as zeros. Kept lower buffer, slot 14 or slot 3 did not
// fault in the game.
//
// kgsl hands out non-SVM GPU VA from a hint that only grows while the highest
// object stays allocated, so a 4 KiB probe allocation tells where the next object
// goes. Each trial builds this layout in a fresh 64 KiB block:
//
//   [pre: rest of the previous block, kept] [block: low (kept) | freed | T ...]
//
// T is the target buffer (GPU read-only, write-combined, like Turnip command
// buffers). It is filled with valid PM4 (CP_NOP with one payload dword, payload =
// marker | dword index) and read by the CP for --life-ms while a churn thread
// allocates and frees other objects (each free makes kgsl invalidate the GPU TLB).
//
// Read paths (--mode, comma separated):
//   ib1  T is submitted as an IB1 (BV and BR both fetch it).
//   ib2  an IB1 in another buffer calls T with CP_INDIRECT_BUFFER.
//   m2m  CP_MEM_TO_MEM copies the first two dwords of every 64-byte line of T
//        into a result buffer; the host compares them. A zero read here is
//        silent: no GPU fault, the run keeps counting.
// An IB fetch that reads a zero header raises "CP opcode error | opcode=0" (BR) or
// "CP BV opcode error" and kgsl reports the fault on the next ioctl.
//
// Exit status: 0 no fault or mismatch, 2 GPU fault, 3 m2m mismatch, 4 a submission did not
// retire within 20 s without a KGSL fault (with --keep-going the run continues and the status
// reflects the first event), 1 error.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define __user
#include "msm_kgsl.h"

// ---- PM4 (type 7 packets, A6XX and later) ----

enum {
    CP_NOP = 0x10,
    CP_THREAD_CONTROL = 0x17,
    CP_MEM_WRITE = 0x3d,
    CP_INDIRECT_BUFFER = 0x3f,
    CP_MEM_TO_MEM = 0x73,
};

static uint32_t odd_parity(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0xf;
    return (~0x6996u >> v) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt) {
    return 0x70000000u | cnt | (odd_parity(cnt) << 15) | ((op & 0x7f) << 16) |
           (odd_parity(op) << 23);
}

#define T_MARKER 0xa5000000u

// ---- configuration ----

static struct {
    int slot;            // slot of T's first page in its 64 KiB block, -1 = no placement
    int low_keep;        // slots [0, low_keep) of the block hold a kept buffer
    bool keep_freed;     // do not free the buffer below T (control)
    bool align64k;       // allocate T with 64 KiB VA alignment instead (workaround check)
    uint64_t t_size;
    uint32_t from, to;   // byte range of T that is read
    bool mode_ib1, mode_ib2, mode_m2m;
    int thread;          // CP_THREAD_CONTROL value at the start of the side IB, 0 = none
    int life_ms;
    int inflight;
    int seconds;
    bool churn;          // churn thread on
    int churn_us;        // pause between churn operations
    int churn_max;
    int prio;            // kgsl priority of the main context (0 = kgsl default)
    int busy;            // busy context IB size in dwords, 0 = off
    int busy_prio;
    bool busy_only;      // run only the busy loop (second process: page table switches)
    int busy_period_us;  // busy loop: wait for each IB, then pause (0 = back to back)
    uint32_t vbo_mib;    // sparse virtual BO with the zero page mapped (like Turnip sparse residency)
    int vbo_bind_us;     // bind/unbind thread pause, -1 = no thread
    uint32_t vbo_reads;  // side IB reads of random dwords of the VBO
    bool vbo_fill;       // bind one child BO over the whole VBO (reads hit normal pages, not PRR)
    bool t_rw;           // T is GPU-writable; the side IB writes a marker into slots 6 and 7
    uint32_t thrash_mib, thrash_reads;            // main side IB: reads of random pages
    uint32_t busy_thrash_mib, busy_thrash_reads;  // busy IB: same, fixed pages
    bool keep_going;
    int report_s;
    unsigned seed;
} cfg = {
    .slot = 6,
    .t_size = 64 * 1024,
    .from = 0,
    .to = 0x2000,
    .mode_ib1 = true,
    .life_ms = 2000,
    .inflight = 4,
    .seconds = 60,
    .churn = true,
    .churn_us = 200,
    .vbo_bind_us = -1,
    .churn_max = 32,
    .report_s = 5,
    .seed = 1,
};

#define F_WC ((uint64_t)KGSL_CACHEMODE_WRITECOMBINE << KGSL_CACHEMODE_SHIFT)
#define F_WB ((uint64_t)KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT)
#define F_RO ((uint64_t)KGSL_MEMFLAGS_GPUREADONLY)
#define F_ALIGN64K ((uint64_t)16 << KGSL_MEMALIGN_SHIFT)
#define BLOCK 0x10000ull
#define PAGE 0x1000ull

// ---- utilities ----

static int g_fd = -1;
static volatile sig_atomic_t g_stop;
static uint64_t g_t0;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static double since_s(uint64_t t) { return (double)(now_ns() - t) / 1e9; }

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) {
    pthread_mutex_lock(&g_log_lock);
    printf("[%9.3f] ", since_s(g_t0));
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&g_log_lock);
}

static int xioctl(unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(g_fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static long read_sysfs_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static long faultcount(void) {
    return read_sysfs_long("/sys/class/kgsl/kgsl-3d0/snapshot/faultcount");
}

static uint32_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// ---- kgsl objects ----

struct bo {
    uint32_t id;
    uint64_t gpuaddr, size, va_len;
    uint32_t *map;
};

// Serializes every kgsl VA allocation and free in this process so that the probe
// predicts the next address. kgsl's VA hint is per process.
static pthread_mutex_t g_va_lock = PTHREAD_MUTEX_INITIALIZER;

static int bo_alloc(struct bo *b, uint64_t size, uint64_t flags) {
    memset(b, 0, sizeof(*b));
    struct kgsl_gpuobj_alloc a = {.size = size, .flags = flags};
    if (xioctl(IOCTL_KGSL_GPUOBJ_ALLOC, &a))
        return -errno;
    struct kgsl_gpuobj_info info = {.id = a.id};
    if (xioctl(IOCTL_KGSL_GPUOBJ_INFO, &info)) {
        int e = errno;
        struct kgsl_gpumem_free_id f = {.id = a.id};
        xioctl(IOCTL_KGSL_GPUMEM_FREE_ID, &f);
        return -e;
    }
    b->id = a.id;
    b->gpuaddr = info.gpuaddr;
    b->size = a.mmapsize ? a.mmapsize : size;
    b->va_len = info.va_len ? info.va_len : b->size;
    return 0;
}

static int bo_map(struct bo *b) {
    void *m = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd,
                   (off_t)b->id << 12);
    if (m == MAP_FAILED)
        return -errno;
    b->map = m;
    return 0;
}

static void bo_free(struct bo *b) {
    if (!b->id)
        return;
    if (b->map)
        munmap(b->map, b->size);
    struct kgsl_gpumem_free_id f = {.id = b->id};
    xioctl(IOCTL_KGSL_GPUMEM_FREE_ID, &f);
    memset(b, 0, sizeof(*b));
}

// Same context flags as Turnip; kgsl requires PREAMBLE and NO_GMEM_ALLOC. Only IBs
// flagged KGSL_CMDLIST_CTXTSWITCH_PREAMBLE are ever skipped, plain IBs always run.
static int ctx_create(uint32_t prio, uint32_t *id) {
    struct kgsl_drawctxt_create c = {
        .flags = KGSL_CONTEXT_SAVE_GMEM | KGSL_CONTEXT_NO_GMEM_ALLOC | KGSL_CONTEXT_PREAMBLE |
                 ((prio << KGSL_CONTEXT_PRIORITY_SHIFT) & KGSL_CONTEXT_PRIORITY_MASK),
    };
    const uint32_t requested = c.flags;
    if (xioctl(IOCTL_KGSL_DRAWCTXT_CREATE, &c))
        return -errno;
    *id = c.drawctxt_id;
    logf_("context %u requested_flags=0x%x returned_flags=0x%x priority=%u", c.drawctxt_id,
          requested, c.flags, (c.flags & KGSL_CONTEXT_PRIORITY_MASK) >> KGSL_CONTEXT_PRIORITY_SHIFT);
    return 0;
}

static void ctx_destroy(uint32_t id) {
    struct kgsl_drawctxt_destroy d = {.drawctxt_id = id};
    xioctl(IOCTL_KGSL_DRAWCTXT_DESTROY, &d);
}

// Returns 0 and the timestamp, or -errno.
static int submit(uint32_t ctx, const struct kgsl_command_object *cmds, unsigned n,
                  uint32_t *ts) {
    struct kgsl_gpu_command req = {
        .flags = KGSL_CMDBATCH_SUBMIT_IB_LIST,
        .cmdlist = (uintptr_t)cmds,
        .cmdsize = sizeof(*cmds),
        .numcmds = n,
        .context_id = ctx,
    };
    int r;
    do {
        r = ioctl(g_fd, IOCTL_KGSL_GPU_COMMAND, &req);
    } while (r == -1 && errno == EINTR);
    if (r)
        return -errno;
    *ts = req.timestamp;
    return 0;
}

static int wait_ts(uint32_t ctx, uint32_t ts, unsigned timeout_ms) {
    struct kgsl_device_waittimestamp_ctxtid w = {
        .context_id = ctx, .timestamp = ts, .timeout = timeout_ms};
    int r;
    do {
        r = ioctl(g_fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &w);
    } while (r == -1 && errno == EINTR);
    return r ? -errno : 0;
}

// Waits for retirement, up to 20 s. A timeout alone is not a fault: a higher priority context
// can starve this one, and a fault of another process (the KGSL fault count is global) makes
// kgsl recover the GPU and replay this context's work. Only an error from the wait itself
// (this context was invalidated) counts. Returns 0, -ETIMEDOUT or the ioctl error.
static int wait_retire(uint32_t ctx, uint32_t ts, long fc_base) {
    (void)fc_base;
    for (int i = 0; i < 20; i++) {
        int r = wait_ts(ctx, ts, 1000);
        if (r != -ETIMEDOUT)
            return r;
        if (g_stop && i >= 2)
            break;
    }
    return -ETIMEDOUT;
}

static struct kgsl_command_object ib_of(const struct bo *b, uint64_t offset, uint64_t bytes) {
    struct kgsl_command_object o = {
        .gpuaddr = b->gpuaddr + offset,
        .size = bytes,
        .flags = KGSL_CMDLIST_IB,
        .id = b->id,
    };
    return o;
}

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t g_end;  // run deadline (monotonic ns)

static uint32_t xorshift(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// ---- TLB pressure: CP reads of one dword from many pages of a large buffer ----

struct thrash {
    struct bo buf, sink;
    uint32_t pages;
};

static int thrash_init(struct thrash *t, uint32_t mib) {
    memset(t, 0, sizeof(*t));
    if (!mib)
        return 0;
    pthread_mutex_lock(&g_va_lock);
    int r = bo_alloc(&t->buf, (uint64_t)mib << 20, F_WC);
    if (!r)
        r = bo_alloc(&t->sink, PAGE, F_WC);
    pthread_mutex_unlock(&g_va_lock);
    t->pages = mib * 256;
    return r;
}

static void thrash_free(struct thrash *t) {
    pthread_mutex_lock(&g_va_lock);
    bo_free(&t->buf);
    bo_free(&t->sink);
    pthread_mutex_unlock(&g_va_lock);
}

static uint32_t *emit_thrash(uint32_t *p, const struct thrash *t, uint32_t reads, uint32_t *rng) {
    for (uint32_t i = 0; i < reads; i++) {
        const uint64_t src = t->buf.gpuaddr + (uint64_t)(xorshift(rng) % t->pages) * PAGE;
        const uint64_t dst = t->sink.gpuaddr + (uint64_t)(i & 1023) * 4;
        *p++ = pkt7(CP_MEM_TO_MEM, 5);
        *p++ = 0;
        *p++ = (uint32_t)dst;
        *p++ = (uint32_t)(dst >> 32);
        *p++ = (uint32_t)src;
        *p++ = (uint32_t)(src >> 32);
    }
    return p;
}

// ---- sparse virtual BO (KGSL VBO) ----
//
// Turnip creates a KGSL VBO for every sparse Vulkan resource; with sparse residency the
// unbound ranges map the KGSL zero page, whose physical address is programmed into the SMMU
// PRR registers (ACTLR bit 5): accesses to it return zero without a fault. shadPS4 keeps
// several such buffers (multi-GiB buffer arenas) and binds and unbinds 64 KiB blocks of them
// all the time.

struct vbo {
    uint32_t id;
    uint64_t gpuaddr, size;
};

static struct vbo g_vbo;

static int vbo_create(struct vbo *v, uint64_t size) {
    memset(v, 0, sizeof(*v));
    struct kgsl_gpuobj_alloc a = {.size = size, .flags = KGSL_MEMFLAGS_VBO | F_ALIGN64K};
    pthread_mutex_lock(&g_va_lock);
    int r = xioctl(IOCTL_KGSL_GPUOBJ_ALLOC, &a) ? -errno : 0;
    pthread_mutex_unlock(&g_va_lock);
    if (r)
        return r;
    struct kgsl_gpumem_get_info info = {.id = a.id};
    if (xioctl(IOCTL_KGSL_GPUMEM_GET_INFO, &info)) {
        r = -errno;
        struct kgsl_gpuobj_free f = {.id = a.id};
        xioctl(IOCTL_KGSL_GPUOBJ_FREE, &f);
        return r;
    }
    v->id = a.id;
    v->gpuaddr = info.gpuaddr;
    v->size = a.mmapsize ? a.mmapsize : size;
    return 0;
}

static void vbo_destroy(struct vbo *v) {
    if (!v->id)
        return;
    struct kgsl_gpuobj_free f = {.id = v->id};
    pthread_mutex_lock(&g_va_lock);
    xioctl(IOCTL_KGSL_GPUOBJ_FREE, &f);
    pthread_mutex_unlock(&g_va_lock);
    memset(v, 0, sizeof(*v));
}

static int vbo_range(const struct vbo *v, uint32_t op, uint32_t child, uint64_t child_off,
                     uint64_t target_off, uint64_t len) {
    struct kgsl_gpumem_bind_range range = {
        .child_offset = child_off,
        .target_offset = target_off,
        .length = len,
        .child_id = child,
        .op = op,
    };
    struct kgsl_gpumem_bind_ranges req = {
        .ranges = (uint64_t)(uintptr_t)&range,
        .ranges_nents = 1,
        .ranges_size = sizeof(range),
        .id = v->id,
    };
    return xioctl(IOCTL_KGSL_GPUMEM_BIND_RANGES, &req) ? -errno : 0;
}

static void *vbo_main(void *arg) {
    (void)arg;
    enum { CHILDREN = 8, CHILD_SIZE = 1 << 20, MAX_BOUND = 64 };
    struct bo child[CHILDREN];
    pthread_mutex_lock(&g_va_lock);
    int r = 0;
    for (int i = 0; i < CHILDREN && !r; i++)
        r = bo_alloc(&child[i], CHILD_SIZE, F_WC);
    pthread_mutex_unlock(&g_va_lock);
    if (r) {
        logf_("vbo: child alloc failed: %s", strerror(-r));
        return NULL;
    }
    struct {
        uint32_t child;
        uint64_t off, len;
    } bound[MAX_BOUND];
    int n = 0;
    uint64_t binds = 0, unbinds = 0, errors = 0;
    uint32_t rng = cfg.seed * 40503u + 7;
    const uint64_t blocks = g_vbo.size / BLOCK;
    while (!g_stop) {
        if (n < MAX_BOUND && (n == 0 || (xorshift(&rng) & 1))) {
            const uint64_t len = (uint64_t)(1 + xorshift(&rng) % 16) * BLOCK;
            const uint64_t off = (uint64_t)(xorshift(&rng) % (uint32_t)(blocks - 16)) * BLOCK;
            const uint32_t c = xorshift(&rng) % CHILDREN;
            const uint64_t coff = (uint64_t)(xorshift(&rng) % (CHILD_SIZE / BLOCK - 15)) * BLOCK;
            if (vbo_range(&g_vbo, KGSL_GPUMEM_RANGE_OP_BIND, child[c].id, coff, off, len) == 0) {
                bound[n].child = c;
                bound[n].off = off;
                bound[n].len = len;
                n++;
                binds++;
            } else {
                errors++;
            }
        } else if (n > 0) {
            const int i = (int)(xorshift(&rng) % (uint32_t)n);
            if (vbo_range(&g_vbo, KGSL_GPUMEM_RANGE_OP_UNBIND, child[bound[i].child].id, 0,
                          bound[i].off, bound[i].len) == 0)
                unbinds++;
            else
                errors++;
            bound[i] = bound[--n];
        }
        if (cfg.vbo_bind_us)
            usleep((useconds_t)cfg.vbo_bind_us);
    }
    for (int i = 0; i < n; i++)
        vbo_range(&g_vbo, KGSL_GPUMEM_RANGE_OP_UNBIND, child[bound[i].child].id, 0, bound[i].off,
                  bound[i].len);
    pthread_mutex_lock(&g_va_lock);
    for (int i = 0; i < CHILDREN; i++)
        bo_free(&child[i]);
    pthread_mutex_unlock(&g_va_lock);
    logf_("vbo: %" PRIu64 " binds, %" PRIu64 " unbinds, %" PRIu64 " errors", binds, unbinds, errors);
    return NULL;
}

// ---- churn: allocations and frees elsewhere in the address space ----

static void *churn_main(void *arg) {
    (void)arg;
    struct bo *live = calloc((size_t)cfg.churn_max, sizeof(*live));
    int n = 0;
    uint64_t allocs = 0, frees = 0;
    static const uint64_t flag_set[] = {F_WC | F_RO, F_WC, F_WB | KGSL_MEMFLAGS_IOCOHERENT};
    while (!g_stop) {
        pthread_mutex_lock(&g_va_lock);
        if (n < cfg.churn_max && (n == 0 || (rnd() & 1))) {
            uint64_t size = (uint64_t)(1 + rnd() % 64) * PAGE * ((rnd() & 7) == 0 ? 16 : 1);
            if (bo_alloc(&live[n], size, flag_set[rnd() % 3]) == 0) {
                n++;
                allocs++;
            }
        } else if (n > 0) {
            int i = (int)(rnd() % (uint32_t)n);
            bo_free(&live[i]);
            live[i] = live[--n];
            frees++;
        }
        pthread_mutex_unlock(&g_va_lock);
        if (cfg.churn_us)
            usleep((useconds_t)cfg.churn_us);
    }
    pthread_mutex_lock(&g_va_lock);
    for (int i = 0; i < n; i++)
        bo_free(&live[i]);
    pthread_mutex_unlock(&g_va_lock);
    free(live);
    logf_("churn: %" PRIu64 " allocs, %" PRIu64 " frees", allocs, frees);
    return NULL;
}

// ---- busy context: long IBs on a second context (or in a second process) ----
//
// The IB starts with --busy-thrash reads of fixed random pages of its own buffer and is
// padded with CP_NOP to --busy dwords. With --busy-only this runs as its own process, so
// the GPU switches page tables between it and the reproducer.

static void *busy_main(void *arg) {
    (void)arg;
    uint32_t ctx;
    int r = ctx_create((uint32_t)cfg.busy_prio, &ctx);
    if (r) {
        logf_("busy: context create failed: %s", strerror(-r));
        return NULL;
    }
    struct thrash th;
    if (thrash_init(&th, cfg.busy_thrash_mib)) {
        logf_("busy: thrash buffer failed");
        ctx_destroy(ctx);
        return NULL;
    }
    const uint32_t words = (uint32_t)cfg.busy;
    struct bo ib;
    pthread_mutex_lock(&g_va_lock);
    r = bo_alloc(&ib, (uint64_t)words * 4, F_WC | F_RO);
    pthread_mutex_unlock(&g_va_lock);
    if (r || bo_map(&ib)) {
        logf_("busy: alloc failed");
        thrash_free(&th);
        ctx_destroy(ctx);
        return NULL;
    }
    uint32_t rng = cfg.seed * 7919u + 1;
    uint32_t *p = ib.map;
    if (cfg.busy_thrash_mib)
        p = emit_thrash(p, &th, cfg.busy_thrash_reads, &rng);
    while (p < ib.map + words)
        *p++ = pkt7(CP_NOP, 0);
    dsb();
    struct kgsl_command_object cmd = ib_of(&ib, 0, (uint64_t)words * 4);
    uint32_t ts[2] = {0, 0};
    uint64_t subs = 0;
    int err = 0;
    const long fc_base = faultcount();
    while (!g_stop && now_ns() < g_end) {
        int s = (int)(subs & 1);
        if (subs >= 2 && (err = wait_retire(ctx, ts[s], fc_base)))
            break;
        if ((err = submit(ctx, &cmd, 1, &ts[s])))
            break;
        subs++;
        // Periodic mode (like a compositor): let this one finish, then pause.
        if (cfg.busy_period_us) {
            if ((err = wait_retire(ctx, ts[s], fc_base)))
                break;
            usleep((useconds_t)cfg.busy_period_us);
        }
    }
    if (subs && !err)
        wait_retire(ctx, ts[(subs - 1) & 1], fc_base);
    logf_("busy: %" PRIu64 " submissions of %u dwords (%u thrash reads)%s%s", subs, words,
          cfg.busy_thrash_mib ? cfg.busy_thrash_reads : 0, err ? ", stopped: " : "",
          err ? strerror(-err) : "");
    ctx_destroy(ctx);
    pthread_mutex_lock(&g_va_lock);
    bo_free(&ib);
    pthread_mutex_unlock(&g_va_lock);
    thrash_free(&th);
    return NULL;
}

// ---- trial layout ----

struct trial {
    struct bo pre, low, freed, t;
    uint64_t block, target;
    bool hit;
    uint64_t t_alloc_ns, freed_ns;
};

static void trial_release(struct trial *tr) {
    pthread_mutex_lock(&g_va_lock);
    bo_free(&tr->t);
    bo_free(&tr->freed);
    bo_free(&tr->low);
    bo_free(&tr->pre);
    pthread_mutex_unlock(&g_va_lock);
}

static uint64_t t_flags(void) {
    return F_WC | (cfg.t_rw ? 0 : F_RO) | (cfg.align64k ? F_ALIGN64K : 0);
}

// --t-rw markers: submission k writes k into dword 3 of line (k % 64) of each of the first two
// pages of the read range. The m2m reads only look at dwords 0 and 1, dword 3 is a CP_NOP
// payload (harmless for ib1/ib2), and with at most 64 submissions in flight a marker is not
// overwritten before it is checked, so a lost write is not hidden by a later one.
static uint32_t t_mark_off(int page, uint64_t k) {
    return (cfg.from & ~0xfffu) + (uint32_t)page * 0x1000 + (uint32_t)(k % 64) * 64 + 12;
}

static int trial_place(struct trial *tr) {
    memset(tr, 0, sizeof(*tr));
    pthread_mutex_lock(&g_va_lock);
    int r = 0;
    if (cfg.slot < 0 || cfg.align64k) {
        r = bo_alloc(&tr->t, cfg.t_size, t_flags());
        tr->hit = true;
        tr->target = tr->t.gpuaddr;
        tr->block = tr->t.gpuaddr & ~(BLOCK - 1);
        tr->t_alloc_ns = now_ns();
        pthread_mutex_unlock(&g_va_lock);
        return r;
    }
    const uint64_t f = F_WC | F_RO;
    struct bo probe;
    if ((r = bo_alloc(&probe, PAGE, f)))
        goto out;
    const uint64_t next = probe.gpuaddr + probe.va_len;
    tr->block = (next + BLOCK - 1) & ~(BLOCK - 1);
    tr->target = tr->block + (uint64_t)cfg.slot * PAGE;
    bool ok = true;
    if (tr->block > next) {
        r = bo_alloc(&tr->pre, tr->block - next, f);
        ok = ok && !r && tr->pre.gpuaddr == next;
    }
    if (!r && cfg.low_keep > 0) {
        r = bo_alloc(&tr->low, (uint64_t)cfg.low_keep * PAGE, f);
        ok = ok && !r && tr->low.gpuaddr == tr->block;
    }
    if (!r && cfg.slot > cfg.low_keep) {
        r = bo_alloc(&tr->freed, (uint64_t)(cfg.slot - cfg.low_keep) * PAGE, f);
        ok = ok && !r && tr->freed.gpuaddr == tr->block + (uint64_t)cfg.low_keep * PAGE;
    }
    if (!r)
        r = bo_alloc(&tr->t, cfg.t_size, t_flags());
    tr->t_alloc_ns = now_ns();
    tr->hit = ok && !r && tr->t.gpuaddr == tr->target;
    bo_free(&probe);
    if (!r && tr->freed.id && !cfg.keep_freed) {
        bo_free(&tr->freed);
        tr->freed_ns = now_ns();
    }
out:
    pthread_mutex_unlock(&g_va_lock);
    return r;
}

// T contents: CP_NOP with one payload dword per pair; payload carries the dword index.
static void trial_fill(struct trial *tr) {
    const uint32_t words = (uint32_t)(tr->t.size / 4);
    for (uint32_t i = 0; i + 1 < words; i += 2) {
        tr->t.map[i] = pkt7(CP_NOP, 1);
        tr->t.map[i + 1] = T_MARKER | ((i + 1) & 0xffffff);
    }
    dsb();
}

static uint32_t t_expected(uint32_t byte_off) {
    const uint32_t i = byte_off / 4;
    return (i & 1) ? (T_MARKER | (i & 0xffffff)) : pkt7(CP_NOP, 1);
}

// ---- main loop ----

#define SIDE_SLOT 0x8000u   // bytes of side IB per in-flight submission
#define RES_SLOT 0x1000u    // bytes of m2m results per in-flight submission
#define SENTINEL 0xdeadbeefu

struct stats {
    uint64_t trials, hits, misses, submits, m2m_reads, m2m_zero, m2m_other, faults, timeouts;
};

static struct stats st;

static void usage(void) {
    fprintf(stderr,
            "usage: kgsl_slot_repro [options]\n"
            "  --slot N          slot (0-15) of T's first page in its 64 KiB block, -1 = no placement (6)\n"
            "  --low-keep N      slots [0,N) of the block hold a kept buffer; [N,slot) is freed (0)\n"
            "  --keep            keep the buffer below T allocated (control)\n"
            "  --align64k        allocate T 64 KiB aligned (workaround check)\n"
            "  --size BYTES      size of T (65536)\n"
            "  --range FROM:TO   byte range of T that is read (0x0:0x2000)\n"
            "  --mode LIST       ib1, ib2, m2m, comma separated (ib1)\n"
            "  --thread N        CP_THREAD_CONTROL at the start of the side IB: 1 BR, 2 BV, 3 both, 0 none (0)\n"
            "  --life-ms N       lifetime of each T (2000)\n"
            "  --inflight N      submissions in flight (4)\n"
            "  --seconds N       run time (60)\n"
            "  --no-churn        no churn thread (allocations and frees of other buffers)\n"
            "  --churn-us N      pause between churn alloc/free (200)\n"
            "  --churn-max N     live churn buffers (32)\n"
            "  --prio N          kgsl priority of the main context, 0 = kgsl default (0)\n"
            "  --thrash MIB:N    side IB also reads N random pages of a MIB buffer (TLB pressure)\n"
            "  --vbo MIB         sparse virtual BO of MIB with the zero page mapped (0 = none)\n"
            "  --vbo-bind-us N   thread binding/unbinding 64 KiB-1 MiB ranges of the VBO, pausing N us\n"
            "                    (-1 = no thread)\n"
            "  --vbo-reads N     side IB also reads N random dwords of the VBO\n"
            "  --vbo-fill        bind one child BO over the whole VBO, so its reads hit normal pages\n"
            "                    instead of the zero page (allocates VBO-sized memory)\n"
            "  --t-rw            T without GPUREADONLY; the side IB also writes the submission number\n"
            "                    into dword 2 of the first two pages read, checked after retirement\n"
            "  --busy DWORDS     second context submitting IBs of this size, 0 = off (0)\n"
            "  --busy-prio N     kgsl priority of the busy context (0)\n"
            "  --busy-thrash MIB:N  busy IB starts with N reads of fixed random pages of a MIB buffer\n"
            "  --busy-period-us N   busy loop waits for each IB, then sleeps N us (0 = back to back)\n"
            "  --busy-only       run only the busy loop in this process (start it as a second process\n"
            "                    so that the GPU switches page tables), for --seconds\n"
            "  --keep-going      continue after a fault or mismatch (new context)\n"
            "  --seed N          (1)\n");
}

static bool parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED()          \
    do {                \
        if (!v)         \
            return false; \
        i++;            \
    } while (0)
        if (!strcmp(a, "--slot")) { NEED(); cfg.slot = atoi(v); }
        else if (!strcmp(a, "--low-keep")) { NEED(); cfg.low_keep = atoi(v); }
        else if (!strcmp(a, "--keep")) cfg.keep_freed = true;
        else if (!strcmp(a, "--align64k")) cfg.align64k = true;
        else if (!strcmp(a, "--size")) { NEED(); cfg.t_size = strtoull(v, NULL, 0); }
        else if (!strcmp(a, "--range")) {
            NEED();
            char *e;
            cfg.from = (uint32_t)strtoul(v, &e, 0);
            if (*e != ':')
                return false;
            cfg.to = (uint32_t)strtoul(e + 1, NULL, 0);
        } else if (!strcmp(a, "--mode")) {
            NEED();
            cfg.mode_ib1 = strstr(v, "ib1") != NULL;
            cfg.mode_ib2 = strstr(v, "ib2") != NULL;
            cfg.mode_m2m = strstr(v, "m2m") != NULL;
        } else if (!strcmp(a, "--thread")) { NEED(); cfg.thread = atoi(v); }
        else if (!strcmp(a, "--life-ms")) { NEED(); cfg.life_ms = atoi(v); }
        else if (!strcmp(a, "--inflight")) { NEED(); cfg.inflight = atoi(v); }
        else if (!strcmp(a, "--seconds")) { NEED(); cfg.seconds = atoi(v); }
        else if (!strcmp(a, "--no-churn")) cfg.churn = false;
        else if (!strcmp(a, "--churn-us")) { NEED(); cfg.churn_us = atoi(v); }
        else if (!strcmp(a, "--prio")) { NEED(); cfg.prio = atoi(v); }
        else if (!strcmp(a, "--busy-only")) cfg.busy_only = true;
        else if (!strcmp(a, "--vbo")) { NEED(); cfg.vbo_mib = (uint32_t)strtoul(v, NULL, 0); }
        else if (!strcmp(a, "--vbo-bind-us")) { NEED(); cfg.vbo_bind_us = atoi(v); }
        else if (!strcmp(a, "--vbo-reads")) { NEED(); cfg.vbo_reads = (uint32_t)strtoul(v, NULL, 0); }
        else if (!strcmp(a, "--vbo-fill")) cfg.vbo_fill = true;
        else if (!strcmp(a, "--t-rw")) cfg.t_rw = true;
        else if (!strcmp(a, "--busy-period-us")) { NEED(); cfg.busy_period_us = atoi(v); }
        else if (!strcmp(a, "--thrash") || !strcmp(a, "--busy-thrash")) {
            NEED();
            char *e;
            const uint32_t mib = (uint32_t)strtoul(v, &e, 0);
            if (*e != ':')
                return false;
            const uint32_t n = (uint32_t)strtoul(e + 1, NULL, 0);
            if (a[2] == 't') {
                cfg.thrash_mib = mib;
                cfg.thrash_reads = n;
            } else {
                cfg.busy_thrash_mib = mib;
                cfg.busy_thrash_reads = n;
            }
        }
        else if (!strcmp(a, "--churn-max")) { NEED(); cfg.churn_max = atoi(v); }
        else if (!strcmp(a, "--busy")) { NEED(); cfg.busy = atoi(v); }
        else if (!strcmp(a, "--busy-prio")) { NEED(); cfg.busy_prio = atoi(v); }
        else if (!strcmp(a, "--keep-going")) cfg.keep_going = true;
        else if (!strcmp(a, "--seed")) { NEED(); cfg.seed = (unsigned)strtoul(v, NULL, 0); }
        else
            return false;
#undef NEED
    }
    if (cfg.slot > 15 || cfg.low_keep < 0 || (cfg.slot >= 0 && cfg.low_keep > cfg.slot) ||
        cfg.t_size < 0x2000 || (cfg.t_size & (PAGE - 1)) || cfg.from >= cfg.to ||
        cfg.to > cfg.t_size || (cfg.from & 7) || (cfg.to & 7) || cfg.inflight < 1 ||
        cfg.inflight > 64 || !(cfg.mode_ib1 || cfg.mode_ib2 || cfg.mode_m2m) ||
        cfg.churn_max < 1 || cfg.churn_us < 0 || cfg.busy < 0 || cfg.thread < 0 ||
        cfg.thread > 3 || cfg.prio < 0 || cfg.prio > 15 || cfg.busy_prio < 0 || cfg.busy_prio > 15)
        return false;
    if (cfg.busy_only && cfg.busy <= 0)
        return false;
    if (cfg.busy_thrash_mib && (uint64_t)cfg.busy_thrash_reads * 6 > (uint64_t)cfg.busy)
        return false;
    if (cfg.mode_m2m) {
        // Two reads per 64-byte line; results plus the completion marker share a result
        // slot, and each read is a 6-dword packet in the side IB.
        const uint32_t reads = ((cfg.to - cfg.from + 63) / 64) * 2;
        if (reads * 4 + 8 > RES_SLOT)
            return false;
    }
    const uint64_t side_bytes = (cfg.mode_m2m ? ((cfg.to - cfg.from + 63) / 64) * 2 * 24 : 0) +
                                (uint64_t)(cfg.thrash_mib ? cfg.thrash_reads : 0) * 24 +
                                (uint64_t)(cfg.vbo_mib ? cfg.vbo_reads : 0) * 24 + 64;
    if (cfg.vbo_mib && cfg.vbo_mib < 2)
        return false;
    if (side_bytes > SIDE_SLOT)
        return false;
    return true;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

// Builds the side IB for submission k in ring slot s; returns its size in bytes.
static uint32_t g_side_rng = 0x9e3779b9u;

static uint32_t build_side(const struct bo *side, const struct bo *res, const struct trial *tr,
                           const struct thrash *th, int s, uint64_t k) {
    uint32_t *p = side->map + (size_t)s * SIDE_SLOT / 4;
    uint32_t *const start = p;
    const uint64_t res_gpu = res->gpuaddr + (uint64_t)s * RES_SLOT;
    if (cfg.thread) {
        *p++ = pkt7(CP_THREAD_CONTROL, 1);
        *p++ = (uint32_t)cfg.thread;
    }
    if (cfg.mode_m2m) {
        uint32_t *r = res->map + (size_t)s * RES_SLOT / 4;
        unsigned j = 0;
        for (uint32_t off = cfg.from; off < cfg.to; off += 64) {
            for (uint32_t w = 0; w < 2; w++, j++) {
                const uint64_t src = tr->t.gpuaddr + off + w * 4;
                const uint64_t dst = res_gpu + (uint64_t)j * 4;
                r[j] = SENTINEL;
                *p++ = pkt7(CP_MEM_TO_MEM, 5);
                *p++ = 0;
                *p++ = (uint32_t)dst;
                *p++ = (uint32_t)(dst >> 32);
                *p++ = (uint32_t)src;
                *p++ = (uint32_t)(src >> 32);
            }
        }
    }
    if (cfg.thrash_mib)
        p = emit_thrash(p, th, cfg.thrash_reads, &g_side_rng);
    if (cfg.vbo_mib && cfg.vbo_reads) {
        for (uint32_t i = 0; i < cfg.vbo_reads; i++) {
            const uint64_t src = g_vbo.gpuaddr + (uint64_t)(xorshift(&g_side_rng) %
                                                            (uint32_t)(g_vbo.size / 4)) * 4;
            const uint64_t dst = th->sink.gpuaddr + (uint64_t)(i & 1023) * 4;
            *p++ = pkt7(CP_MEM_TO_MEM, 5);
            *p++ = 0;
            *p++ = (uint32_t)dst;
            *p++ = (uint32_t)(dst >> 32);
            *p++ = (uint32_t)src;
            *p++ = (uint32_t)(src >> 32);
        }
    }
    if (cfg.t_rw) {
        for (int pg = 0; pg < 2; pg++) {
            const uint64_t a = tr->t.gpuaddr + t_mark_off(pg, k);
            *p++ = pkt7(CP_MEM_WRITE, 3);
            *p++ = (uint32_t)a;
            *p++ = (uint32_t)(a >> 32);
            *p++ = (uint32_t)k;
        }
    }
    if (cfg.mode_ib2) {
        const uint64_t a = tr->t.gpuaddr + cfg.from;
        *p++ = pkt7(CP_INDIRECT_BUFFER, 3);
        *p++ = (uint32_t)a;
        *p++ = (uint32_t)(a >> 32);
        *p++ = (cfg.to - cfg.from) / 4;
    }
    // Completion marker: the submission number, in the second half of the result slot.
    const uint64_t seq = res_gpu + RES_SLOT - 8;
    *p++ = pkt7(CP_MEM_WRITE, 3);
    *p++ = (uint32_t)seq;
    *p++ = (uint32_t)(seq >> 32);
    *p++ = (uint32_t)k;
    return (uint32_t)((p - start) * 4);
}

static uint64_t st_write_lost;

static int check_slot(const struct bo *res, const struct trial *tr, int s, uint64_t k) {
    int bad = 0;
    if (cfg.t_rw) {
        for (int pg = 0; pg < 2; pg++) {
            const uint32_t off = t_mark_off(pg, k);
            const uint32_t got = tr->t.map[off / 4];
            if (got != (uint32_t)k) {
                st_write_lost++;
                bad++;
                const uint64_t va = tr->t.gpuaddr + off;
                logf_("T WRITE LOST submit=%" PRIu64 " va=0x%" PRIx64 " slot=%u off=0x%x got=0x%08x",
                      k, va, (unsigned)((va >> 12) & 0xf), off, got);
            }
        }
    }
    if (!cfg.mode_m2m)
        return bad;
    const uint32_t *r = res->map + (size_t)s * RES_SLOT / 4;
    unsigned j = 0;
    uint32_t zeros[16] = {0}, others = 0, first = 0, last = 0, nbad = 0;
    for (uint32_t off = cfg.from; off < cfg.to; off += 64) {
        for (uint32_t w = 0; w < 2; w++, j++) {
            const uint32_t byte = off + w * 4;
            const uint32_t want = t_expected(byte);
            const uint32_t got = r[j];
            st.m2m_reads++;
            if (got == want)
                continue;
            if (got == 0) {
                st.m2m_zero++;
                zeros[((tr->t.gpuaddr + byte) >> 12) & 0xf]++;
            } else {
                st.m2m_other++;
                others++;
            }
            if (!nbad)
                first = byte;
            last = byte;
            if (nbad++ < 4 && bad < 4) {
                const uint64_t va = tr->t.gpuaddr + byte;
                logf_("M2M MISMATCH submit=%" PRIu64 " va=0x%" PRIx64 " slot=%u off=0x%x got=0x%08x "
                      "want=0x%08x t_age_ms=%.1f freed_age_ms=%.1f",
                      k, va, (unsigned)((va >> 12) & 0xf), byte, got, want,
                      since_s(tr->t_alloc_ns) * 1e3,
                      tr->freed_ns ? since_s(tr->freed_ns) * 1e3 : -1.0);
            }
        }
    }
    if (nbad) {
        char slots[160] = "";
        size_t o = 0;
        for (int i = 0; i < 16; i++)
            if (zeros[i])
                o += (size_t)snprintf(slots + o, sizeof(slots) - o, " %d:%u", i, zeros[i]);
        logf_("M2M BAD submit=%" PRIu64 " T=0x%" PRIx64 " bad=%u zero=%u other=%u off=0x%x..0x%x "
              "zero_per_slot{%s } t_age_ms=%.1f",
              k, tr->t.gpuaddr, nbad, nbad - others, others, first, last, slots,
              since_s(tr->t_alloc_ns) * 1e3);
    }
    return bad + (int)nbad;
}

int main(int argc, char **argv) {
    if (!parse_args(argc, argv)) {
        usage();
        return 1;
    }
    g_t0 = now_ns();
    rng_state = cfg.seed ? cfg.seed : 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) {
        perror("open /dev/kgsl-3d0");
        return 1;
    }
    char model[64] = "?";
    FILE *mf = fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (mf) {
        if (!fgets(model, sizeof(model), mf))
            strcpy(model, "?");
        model[strcspn(model, "\n")] = 0;
        fclose(mf);
    }
    const long fc0 = faultcount();
    g_end = g_t0 + (uint64_t)cfg.seconds * 1000000000ull;
    g_side_rng ^= cfg.seed * 2654435761u;
    if (cfg.busy_only) {
        logf_("kgsl_slot_repro busy-only gpu=%s pid=%d faultcount=%ld busy=%d busy_prio=%d "
              "busy_thrash=%u:%u busy_period_us=%d seconds=%d",
              model, getpid(), fc0, cfg.busy, cfg.busy_prio, cfg.busy_thrash_mib,
              cfg.busy_thrash_reads, cfg.busy_period_us, cfg.seconds);
        busy_main(NULL);
        logf_("done busy-only faultcount %ld -> %ld", fc0, faultcount());
        close(g_fd);
        return 0;
    }
    logf_("kgsl_slot_repro gpu=%s pid=%d faultcount=%ld slot=%d low_keep=%d keep=%d align64k=%d "
          "size=0x%" PRIx64 " range=0x%x:0x%x mode=%s%s%s thread=%d prio=%d life_ms=%d inflight=%d "
          "seconds=%d churn=%d churn_us=%d churn_max=%d thrash=%u:%u busy=%d busy_prio=%d "
          "busy_thrash=%u:%u vbo=%u vbo_bind_us=%d vbo_reads=%u vbo_fill=%d t_rw=%d seed=%u",
          model, getpid(), fc0, cfg.slot, cfg.low_keep, cfg.keep_freed, cfg.align64k, cfg.t_size,
          cfg.from, cfg.to, cfg.mode_ib1 ? "ib1 " : "", cfg.mode_ib2 ? "ib2 " : "",
          cfg.mode_m2m ? "m2m" : "", cfg.thread, cfg.prio, cfg.life_ms, cfg.inflight, cfg.seconds,
          cfg.churn, cfg.churn_us, cfg.churn_max, cfg.thrash_mib, cfg.thrash_reads, cfg.busy,
          cfg.busy_prio, cfg.busy_thrash_mib, cfg.busy_thrash_reads, cfg.vbo_mib, cfg.vbo_bind_us,
          cfg.vbo_reads, cfg.vbo_fill, cfg.t_rw, cfg.seed);

    uint32_t ctx;
    int cr = ctx_create((uint32_t)cfg.prio, &ctx);
    if (cr) {
        fprintf(stderr, "IOCTL_KGSL_DRAWCTXT_CREATE: %s\n", strerror(-cr));
        return 1;
    }
    struct thrash th;
    // The VBO reads use the thrash sink as their destination.
    if (thrash_init(&th, cfg.thrash_mib ? cfg.thrash_mib : (cfg.vbo_reads ? 1 : 0))) {
        perror("thrash buffer");
        return 1;
    }
    if (cfg.vbo_mib) {
        int vr = vbo_create(&g_vbo, (uint64_t)cfg.vbo_mib << 20);
        if (vr) {
            fprintf(stderr, "VBO create: %s\n", strerror(-vr));
            return 1;
        }
        logf_("vbo id=%u gpuaddr=0x%" PRIx64 " size=0x%" PRIx64 " (zero page mapped)", g_vbo.id,
              g_vbo.gpuaddr, g_vbo.size);
    }
    struct bo vbo_child = {0};
    if (cfg.vbo_mib && cfg.vbo_fill) {
        pthread_mutex_lock(&g_va_lock);
        int vr = bo_alloc(&vbo_child, g_vbo.size, F_WC);
        pthread_mutex_unlock(&g_va_lock);
        if (!vr)
            vr = vbo_range(&g_vbo, KGSL_GPUMEM_RANGE_OP_BIND, vbo_child.id, 0, 0, g_vbo.size);
        if (vr) {
            fprintf(stderr, "VBO fill: %s\n", strerror(-vr));
            return 1;
        }
        logf_("vbo filled with child id=%u (0x%" PRIx64 " bytes)", vbo_child.id, g_vbo.size);
    }
    struct bo side, res;
    if (bo_alloc(&side, (uint64_t)cfg.inflight * SIDE_SLOT, F_WC | F_RO) || bo_map(&side) ||
        bo_alloc(&res, (uint64_t)cfg.inflight * RES_SLOT, F_WC) || bo_map(&res)) {
        perror("side/result buffers");
        return 1;
    }
    memset(res.map, 0, res.size);

    pthread_t churn_thread, busy_thread, vbo_thread;
    bool churn_on = cfg.churn, busy_on = cfg.busy > 0, vbo_on = cfg.vbo_mib && cfg.vbo_bind_us >= 0;
    if (vbo_on)
        pthread_create(&vbo_thread, NULL, vbo_main, NULL);
    if (churn_on)
        pthread_create(&churn_thread, NULL, churn_main, NULL);
    if (busy_on)
        pthread_create(&busy_thread, NULL, busy_main, NULL);

    uint32_t *ts = calloc((size_t)cfg.inflight, sizeof(*ts));
    bool *used = calloc((size_t)cfg.inflight, sizeof(*used));
    uint64_t *sub_of = calloc((size_t)cfg.inflight, sizeof(*sub_of));
    uint64_t k = 0, last_report = now_ns();
    long fc_base = fc0;
    int status = 0;
    const uint64_t end = g_end;

    while (!g_stop && now_ns() < end) {
        struct trial tr;
        int r = trial_place(&tr);
        if (r) {
            logf_("placement failed: %s", strerror(-r));
            status = 1;
            break;
        }
        if (!tr.hit) {
            st.misses++;
            trial_release(&tr);
            continue;
        }
        if (bo_map(&tr.t)) {
            logf_("map T failed: %s", strerror(errno));
            trial_release(&tr);
            status = 1;
            break;
        }
        st.hits++;
        st.trials++;
        trial_fill(&tr);
        if (st.trials <= 3)
            logf_("trial %" PRIu64 ": T=0x%" PRIx64 " (block 0x%" PRIx64 " slot %u) pre=0x%" PRIx64
                  " low=0x%" PRIx64 " freed_below=%s",
                  st.trials, tr.t.gpuaddr, tr.block, (unsigned)((tr.t.gpuaddr >> 12) & 0xf),
                  tr.pre.gpuaddr, tr.low.gpuaddr,
                  cfg.slot > cfg.low_keep && !cfg.align64k && cfg.slot >= 0
                      ? (cfg.keep_freed ? "kept" : "yes")
                      : "no");

        const uint64_t t_end = tr.t_alloc_ns + (uint64_t)cfg.life_ms * 1000000ull;
        uint64_t subs_on_t = 0;
        bool fault = false, hang = false;
        while (!g_stop && now_ns() < t_end && now_ns() < end) {
            const int s = (int)(k % (uint64_t)cfg.inflight);
            if (used[s]) {
                int w = wait_retire(ctx, ts[s], fc_base);
                if (w) {
                    fault = true;
                    hang = w == -ETIMEDOUT;
                    logf_("GPU %s (wait) errno=%d (%s) ctx=%u ts=%u submit=%" PRIu64,
                          hang ? "TIMEOUT" : "FAULT", -w, strerror(-w), ctx, ts[s], k);
                    break;
                }
                used[s] = false;
                if (check_slot(&res, &tr, s, sub_of[s]) && !cfg.keep_going) {
                    status = 3;
                    g_stop = 1;
                    break;
                }
            }
            const uint32_t side_bytes = build_side(&side, &res, &tr, &th, s, k);
            dsb();
            struct kgsl_command_object cmds[2];
            unsigned n = 0;
            if (cfg.mode_ib1)
                cmds[n++] = ib_of(&tr.t, cfg.from, cfg.to - cfg.from);
            cmds[n++] = ib_of(&side, (uint64_t)s * SIDE_SLOT, side_bytes);
            r = submit(ctx, cmds, n, &ts[s]);
            if (r) {
                fault = true;
                logf_("GPU FAULT (submit) errno=%d (%s) ctx=%u submit=%" PRIu64, -r, strerror(-r),
                      ctx, k);
                break;
            }
            used[s] = true;
            sub_of[s] = k;
            k++;
            subs_on_t++;
            st.submits++;
        }
        // Retire everything that still reads T before it is freed.
        for (int i = 0; i < cfg.inflight && !fault; i++) {
            const int s = (int)((k + (uint64_t)i) % (uint64_t)cfg.inflight);
            if (!used[s])
                continue;
            int w = wait_retire(ctx, ts[s], fc_base);
            if (w) {
                fault = true;
                hang = w == -ETIMEDOUT;
                logf_("GPU %s (drain) errno=%d (%s) ctx=%u ts=%u", hang ? "TIMEOUT" : "FAULT", -w,
                      strerror(-w), ctx, ts[s]);
                break;
            }
            used[s] = false;
            if (check_slot(&res, &tr, s, sub_of[s]) && !cfg.keep_going) {
                status = 3;
                g_stop = 1;
            }
        }
        if (fault) {
            if (!hang)
                st.faults++;
            usleep(200000);
            const long fc = faultcount();
            logf_("FAULT DETAIL T=0x%" PRIx64 " block=0x%" PRIx64 " slot=%u range=0x%x:0x%x "
                  "t_age_ms=%.1f freed_age_ms=%.1f submits_on_T=%" PRIu64 " total_submits=%" PRIu64
                  " trial=%" PRIu64 " faultcount %ld -> %ld",
                  tr.t.gpuaddr, tr.block, (unsigned)((tr.t.gpuaddr >> 12) & 0xf), cfg.from, cfg.to,
                  since_s(tr.t_alloc_ns) * 1e3, tr.freed_ns ? since_s(tr.freed_ns) * 1e3 : -1.0,
                  subs_on_t, st.submits, st.trials, fc0, fc);
            if (hang)
                st.timeouts++;
            status = status ? status : (hang && fc == fc_base ? 4 : 2);
            fc_base = fc;
            // The context is invalidated by kgsl after a fault; outstanding work is gone.
            ctx_destroy(ctx);
            memset(used, 0, (size_t)cfg.inflight * sizeof(*used));
            if (!cfg.keep_going) {
                trial_release(&tr);
                g_stop = 1;
                break;
            }
            if ((cr = ctx_create((uint32_t)cfg.prio, &ctx))) {
                logf_("context re-create failed: %s", strerror(-cr));
                trial_release(&tr);
                g_stop = 1;
                break;
            }
        }
        trial_release(&tr);

        if (now_ns() - last_report >= (uint64_t)cfg.report_s * 1000000000ull) {
            last_report = now_ns();
            logf_("progress trials=%" PRIu64 " misses=%" PRIu64 " submits=%" PRIu64
                  " m2m_reads=%" PRIu64 " m2m_zero=%" PRIu64 " m2m_other=%" PRIu64
                  " faults=%" PRIu64 " timeouts=%" PRIu64 " gpu_busy=%ld%% gpuclk=%ld faultcount=%ld",
                  st.trials, st.misses, st.submits, st.m2m_reads, st.m2m_zero, st.m2m_other,
                  st.faults, st.timeouts, read_sysfs_long("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"),
                  read_sysfs_long("/sys/class/kgsl/kgsl-3d0/gpuclk"), faultcount());
        }
    }

    g_stop = 1;
    if (churn_on)
        pthread_join(churn_thread, NULL);
    if (busy_on)
        pthread_join(busy_thread, NULL);
    if (vbo_on)
        pthread_join(vbo_thread, NULL);
    logf_("done trials=%" PRIu64 " misses=%" PRIu64 " submits=%" PRIu64 " m2m_reads=%" PRIu64
          " m2m_zero=%" PRIu64 " m2m_other=%" PRIu64 " faults=%" PRIu64 " timeouts=%" PRIu64
          " write_lost=%" PRIu64 " faultcount %ld -> %ld status=%d",
          st.trials, st.misses, st.submits, st.m2m_reads, st.m2m_zero, st.m2m_other, st.faults,
          st.timeouts, st_write_lost, fc0, faultcount(), status);
    if (status == 0 && (st.m2m_zero || st.m2m_other || st_write_lost))
        status = 3;
    if (status == 0 && st.faults)
        status = 2;
    ctx_destroy(ctx);
    bo_free(&side);
    bo_free(&res);
    thrash_free(&th);
    if (vbo_child.id)
        vbo_range(&g_vbo, KGSL_GPUMEM_RANGE_OP_UNBIND, vbo_child.id, 0, 0, g_vbo.size);
    vbo_destroy(&g_vbo);
    pthread_mutex_lock(&g_va_lock);
    bo_free(&vbo_child);
    pthread_mutex_unlock(&g_va_lock);
    close(g_fd);
    return status;
}
