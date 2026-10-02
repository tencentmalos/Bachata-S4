// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne 1.00 (CUSA03023) frame manager, as used by the 60 FPS package.
//
// Singleton pointer at eboot+0x55404c8, object of 0x2c8 bytes built at
// eboot+0x2035870. eboot+0x2035ac0 runs once per frame from the main step
// (eboot+0x201a0d0): it picks a pacing mode, sets the target frame time, sleeps
// (gettimeofday/usleep) until that time has passed since the last frame, and
// stores the measured duration in +0x264. Names describe observed behaviour;
// they are not the game's symbols. Unnamed bytes are deliberately opaque.
#pragma once

typedef struct BbFrameManager {
    unsigned char unknown_000[0x08];
    // +0x008: pacing mode 0..4 (5-entry jump table at eboot+0x20362f8). Mode 2 is
    // the game's own 60 Hz mode (interval 1, 1/60); the others pace at 1/30.
    int pacing_mode;
    // +0x00c: used instead of pacing_mode while the byte at +0x276 is set.
    int pacing_mode_override;
    // +0x010: vblanks per flip (1 or 2), and a byte flag set together with it.
    int vsync_interval;
    unsigned char vsync_flag;
    unsigned char unknown_015[3];
    // +0x018: seconds the pacer waits per frame (0x3d088889 = 1/30 by default).
    float target_frame_time;
    unsigned char unknown_01c[0x264 - 0x01c];
    // +0x264: measured duration of the last frame in seconds.
    float frame_delta;
    // +0x268, +0x26c: written together as one qword by every pacing mode
    // (mode 0/3: 0x1e_00000001, modes 1/2/4: 0x1e_00000000).
    unsigned pacing_window[2];
    unsigned char unknown_270[0x2c8 - 0x270];
} BbFrameManager;

#ifdef __cplusplus
static_assert(sizeof(BbFrameManager) == 0x2c8);
static_assert(__builtin_offsetof(BbFrameManager, pacing_mode) == 0x08);
static_assert(__builtin_offsetof(BbFrameManager, vsync_interval) == 0x10);
static_assert(__builtin_offsetof(BbFrameManager, vsync_flag) == 0x14);
static_assert(__builtin_offsetof(BbFrameManager, target_frame_time) == 0x18);
static_assert(__builtin_offsetof(BbFrameManager, frame_delta) == 0x264);
static_assert(__builtin_offsetof(BbFrameManager, pacing_window) == 0x268);
#endif
