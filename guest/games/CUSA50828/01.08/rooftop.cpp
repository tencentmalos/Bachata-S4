// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit coarse phases, exact TMNT 01.08 module only. Stack-local timing is
// reentrant and thread-safe; no shared start slot or guessed engine structure.
#include "shad_imports.h"
static void report(shad_u64 id, shad_u64 start, shad_u64 end) {
  // Both reports are outside the measured original call. The timestamp VALUE,
  // not counter emission time, aligns the interval with host monotonic PROF.
  shad_sdk_counter(id, (shad_i64)start);
  shad_sdk_counter(id + 1, (shad_i64)(end - start));
}
extern "C" shad_u64 patch_frame(void* self) {
  const auto start = shad_sdk_clock_ns();
  const auto result = original_frame(self);
  const auto end = shad_sdk_clock_ns();
  report(1, start, end);
  return result;
}
extern "C" shad_u64 patch_begin(void) {
  const auto start = shad_sdk_clock_ns();
  const auto result = original_begin();
  const auto end = shad_sdk_clock_ns();
  report(3, start, end);
  return result;
}
extern "C" shad_u64 patch_submit(void) {
  const auto start = shad_sdk_clock_ns();
  const auto result = original_submit();
  const auto end = shad_sdk_clock_ns();
  report(5, start, end);
  return result;
}
extern "C" shad_u64 patch_jobs(void* queue) {
  const auto start = shad_sdk_clock_ns();
  const auto result = original_jobs(queue);
  const auto end = shad_sdk_clock_ns();
  report(7, start, end);
  return result;
}
extern "C" int patch_fmod(void* self) {
  const auto start = shad_sdk_clock_ns();
  const auto result = original_fmod(self);
  const auto end = shad_sdk_clock_ns();
  report(9, start, end);
  return result;
}
