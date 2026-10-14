// SPDX-License-Identifier: GPL-2.0-or-later
// KINGDOM HEARTS III (CUSA15072): the game thread sleeps through the frame-rate cap's last
// stretch instead of yielding in a loop.
//
// UE4's UEngine::UpdateTimeAndHandleMaxTickRate (eboot+0x5d8dfc0, UnrealEngine.cpp) holds a
// frame to the maximum tick rate. With time left over it sleeps (WaitTime - 2 ms) when that is
// more than 5 ms, then waits for the rest in a loop that calls scePthreadYield until the end
// time (a double at rbp-0x40, in the units of sceKernelGetProcessTimeCounter times the game's
// seconds-per-cycle global) has passed: 2 ms of yielding after a sleep, the whole wait when it is
// 5 ms or less. The 2 ms are slack for a PC scheduler; each yield here is an HLE round trip and a
// system call, about 26 thousand a second on the game thread in the Mount Olympus scene.
//
// The site replaces that `call scePthreadYield` (eboot+0x5d8e2ea): while more than
// kMinSleepSeconds are left it sleeps all but kSpinSeconds of them with the game's own
// sceKernelUsleep; closer to the end it yields as before. The loop's end check is unchanged.

namespace {
constexpr double kSpinSeconds = 0.0002;
constexpr double kMinSleepSeconds = 0.0004;
constexpr shad_u64 kSleepLog = 2;
constexpr shad_u64 kSleepLogEvery = 4096;

shad_u64 g_tick_sleeps;
} // namespace

extern "C" void kh3_tick_wait_round(ShadSiteContext* ctx) {
    const double end = *reinterpret_cast<const double*>(ctx->gpr->rbp - 0x40);
    const double now =
        static_cast<double>(kh3_sceKernelGetProcessTimeCounter()) * *kh3_seconds_per_cycle;
    const double left = end - now;
    if (!(left > kMinSleepSeconds)) {
        kh3_scePthreadYield();
        return;
    }
    kh3_sceKernelUsleep(static_cast<unsigned int>((left - kSpinSeconds) * 1e6));
    const shad_u64 sleeps = __atomic_add_fetch(&g_tick_sleeps, 1, __ATOMIC_RELAXED);
    if (sleeps % kSleepLogEvery == 0)
        shad::Log(kSleepLog, static_cast<shad_i64>(sleeps));
}
