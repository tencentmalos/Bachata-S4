// SPDX-License-Identifier: GPL-2.0-or-later
// ASTRO BOT Rescue Mission (CUSA12392): its world advances by the time its frames take.
//
// The title advances its world by one sixtieth of a second for every frame it draws, however
// long the frame took: on anything slower than the console, the whole game runs in slow motion
// (half speed at 30 frames a second). Its engine keeps that step in three places, written once by
// the function that sets its frame rate and read wherever time passes. This package writes them
// before every frame is handed in (sceGnmSubmitDone) with what frames really take.
//
// Algorithm and addresses: AstroQuest v0.18 (9ff3e43), shadps4-arm64-main/src/core/known_title.cpp
// (TimeStep, OnFrameSubmitted) and known_title_builds.h (GPL-2.0-or-later). The same source builds
// for each known build; the recipes (01.00/, 01.04/) give the addresses and the signatures that
// tell the build apart.

namespace {
// The title's own step, a sixtieth of a second.
constexpr double kNominal = 1.0 / 60.0;
// The longest step made up for: below 20 frames a second the game slows down again rather than
// jumping ahead in large steps (AstroQuest's default).
constexpr double kLongest = 1.0 / 20.0;
// A frame longer than this is a stall (loading, a breakpoint): it does not count.
constexpr double kStall = 0.25;
// How much of the difference to the last frame goes into what frames are taken to take. A step
// that followed every frame would be wrong twice over where long and short frames alternate.
constexpr double kFollow = 0.15;
// Real time the game's clock has fallen behind (or run ahead) by, at most, and how much of it a
// frame makes up for: this keeps what is seen in step with what is heard.
constexpr double kMostOwed = 0.1;
constexpr double kRepay = 0.1;

constexpr shad_u64 kStepCounter = 1;
constexpr shad_u64 kFrameCounter = 2;
constexpr shad_u64 kStepLog = 1;

double Clamp(double v, double low, double high) {
    return v < low ? low : v > high ? high : v;
}
double Abs(double v) {
    return v < 0 ? -v : v;
}

struct TimeStep {
    double average = kNominal;
    double step = kNominal;
    double owed = 0.0;

    double Next(double frame) {
        if (frame > kStall)
            return step;
        average += (frame - average) * kFollow;
        owed = Clamp(owed + frame - step, -kMostOwed, kMostOwed);
        step = Clamp(average + owed * kRepay, kNominal, kLongest);
        // At the rate the title was made for, exactly what it would use itself.
        if (Abs(average - kNominal) < 0.0004 && Abs(owed) < 0.004) {
            step = kNominal;
            owed = 0.0;
        }
        return step;
    }
};

// Frames are handed in from one render thread.
TimeStep g_time_step;
shad_u64 g_last_ns;
shad_u64 g_frames;
} // namespace

extern "C" int astro_submit_done(void) {
    const shad_u64 now = shad_sdk_clock_ns();
    if (g_last_ns != 0) {
        const double frame = static_cast<double>(now - g_last_ns) * 1e-9;
        const double step = g_time_step.Next(frame);
        *astro_frame_rate = 1.0 / step;
        *astro_frame_seconds = static_cast<float>(step);
        *astro_frame_microseconds = static_cast<shad_u64>(step * 1e6);
        shad_sdk_counter(kFrameCounter, static_cast<shad_i64>(frame * 1e6));
        shad_sdk_counter(kStepCounter, static_cast<shad_i64>(step * 1e6));
        if (++g_frames % 600 == 0)
            shad::Log(kStepLog, static_cast<shad_i64>(step * 1e6));
    }
    g_last_ns = now;
    return astro_sceGnmSubmitDone();
}
