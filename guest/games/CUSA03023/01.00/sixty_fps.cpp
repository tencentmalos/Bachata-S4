// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne 1.00 (CUSA03023) 60 FPS, as C++ site handlers.
//
// Port of the idea behind Lance McDonald's 60 FPS patch for 1.09 to the 1.00
// eboot: run the frame pacer at 1/60 s, and where gameplay code assumed a fixed
// 1/30 s per frame, use the frame manager's measured frame time instead. Every
// handler replaces exactly one original instruction (sites in "replace" mode, see
// sixty_fps.recipe.json); disabling a site at runtime runs that instruction again.
//
// Handlers run on the guest thread at the site, with the guest's registers in
// `ctx`. Writes to ctx->gpr / ctx->ymm become the guest's registers on return.

namespace {
// The pacer's 60 Hz frame time: same constant (0x3c888889) as its own mode 2.
constexpr float kTargetFrameTime = 1.0f / 60.0f;
// The qword Lance McDonald's 1.09 patch stores to +0x268 for 60 FPS
// (+0x268 = 0, +0x26c = 1). Its meaning is not established.
constexpr shad_u64 kPacingWindow = 0x0000000100000000ull;
// Declared in the recipe: per-frame measured frame time, in microseconds.
constexpr shad_u64 kFrameDeltaCounter = 1;

const BbFrameManager* FrameManager() {
    return *bb_frame_manager_slot;
}
void StoreFloat(shad_u64 address, float value) {
    *reinterpret_cast<float*>(address) = value;
}
} // namespace

extern "C" {
// Frame pacer (eboot+0x2035ac0), r12 = frame manager.
// Replaces `mov dword [r12+0x18], 1/30` of pacing modes 0/3 and 1/4.
void bb60_pacer_target_frame_time(ShadSiteContext* ctx) {
    auto* fm = reinterpret_cast<BbFrameManager*>(ctx->gpr->r12);
    fm->target_frame_time = kTargetFrameTime;
}

// Frame pacer: replaces `movabs rcx, <window>`; the next instruction stores rcx
// to +0x268. The modes 1/2/4 instance is also the target of mode 2's jump.
void bb60_pacer_window(ShadSiteContext* ctx) {
    ctx->gpr->rcx = kPacingWindow;
}

// Main step (eboot+0x201a0d0), right after the pacer ran: replaces
// `mov dword [rbp-0x38], 1/30`, the time field of the {ptr, float} block passed
// to eboot+0x2052690. Game logic driven from there advances by the real frame time.
void bb60_main_step_delta(ShadSiteContext* ctx) {
    const float delta = FrameManager()->frame_delta;
    StoreFloat(ctx->gpr->rbp - 0x38, delta);
    shad_sdk_counter(kFrameDeltaCounter, static_cast<shad_i64>(delta * 1e6f));
}

// eboot+0x17f9b40: replaces `mov dword [rbp-0x48], 1/30`, the time field of the
// local block it fills before calling the object's update (vtable slot +0x48).
void bb60_update_17f9b40_delta(ShadSiteContext* ctx) {
    StoreFloat(ctx->gpr->rbp - 0x48, FrameManager()->frame_delta);
}

// SprjWorldAiManager frame bookkeeping (eboot+0x1dbc4f8): replaces
// `vmulss xmm0, xmm0, [1/30]`, which turns a frame count into seconds. With a
// variable frame rate one frame lasts frame_delta seconds. As a VEX.128
// instruction it also clears the upper half of ymm0.
void bb60_ai_frames_to_seconds(ShadSiteContext* ctx) {
    float* xmm0 = shad_site_xmm_f32(ctx, 0);
    *xmm0 = *xmm0 * FrameManager()->frame_delta;
    shad_site_zero_upper(ctx, 0);
}
}
