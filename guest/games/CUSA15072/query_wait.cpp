// SPDX-License-Identifier: GPL-2.0-or-later
// KINGDOM HEARTS III (CUSA15072): the render thread sleeps while it waits for GPU query results.
//
// UE4's GnmRHI reads occlusion and timestamp query results in RHIGetRenderQueryResult
// (eboot+0x3955d0). When a result is not there yet and the caller asks to wait, it calls
// eboot+0x3a0d10, which polls: run the render thread's queued tasks, test the words the GPU
// writes for the query (the begin/end counters of all 8 render backends, 16 on Pro, have their
// valid bit; or the timestamp is non-zero), check a timeout, and go round again. It never
// sleeps. Here the query words are written when the GPU command processor thread reaches the
// query's packets, which in a GPU-bound scene is a large part of a frame later, so the render
// thread spends that time spinning on a big core.
//
// The site sits on the loop's back edge (eboot+0x3a0eee, `cmp byte [r15], 0; jz loop`, r15 =
// the caller's result flag the loop has just written). When the result is still missing, it
// sleeps kSleepMicroseconds with the game's own sceKernelUsleep before the next round. The
// timeout check before it and the work inside the loop are unchanged; a result is seen at most
// one sleep later than before.

namespace {
// One round of the loop without the sleep takes a few microseconds; the wait it ends is a frame
// or less. The host sleep has some 50 us of timer slack on top of this.
constexpr shad_u64 kSleepMicroseconds = 100;
constexpr shad_u64 kSleepLog = 1;
constexpr shad_u64 kSleepLogEvery = 16384;

shad_u64 g_sleeps;
} // namespace

extern "C" void kh3_query_wait_sleep(ShadSiteContext* ctx) {
    const auto* ready = reinterpret_cast<const unsigned char*>(ctx->gpr->r15);
    if (*ready != 0)
        return;
    kh3_sceKernelUsleep(static_cast<unsigned int>(kSleepMicroseconds));
    const shad_u64 sleeps = __atomic_add_fetch(&g_sleeps, 1, __ATOMIC_RELAXED);
    if (sleeps % kSleepLogEvery == 0)
        shad::Log(kSleepLog, static_cast<shad_i64>(sleeps));
}
