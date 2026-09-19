// SPDX-License-Identifier: GPL-2.0-or-later
// FMOD owns these opaque guest objects. Preserve every argument and return
// code.
#include "shad_imports.h"
static shad_u64 update_calls;
extern "C" int patch_update(void *self) {
  const auto n = __atomic_add_fetch(&update_calls, 1, __ATOMIC_RELAXED);
  // Explicit author-selected 1/16 sampling; all other calls stay in FEX.
  if ((n & 15) != 1)
    return original_update(self);
  shad_sdk_counter(4, n);
  int result;
  {
    shad::TimedScope time(1);
    result = original_update(self);
  }
  shad_sdk_counter(5, result);
  return result;
}
extern "C" int patch_flush_commands(void *self) {
  int result;
  {
    shad::TimedScope time(2);
    result = original_flush_commands(self);
  }
  shad_sdk_counter(6, result);
  return result;
}
extern "C" int patch_flush_samples(void *self) {
  int result;
  {
    shad::TimedScope time(3);
    result = original_flush_samples(self);
  }
  shad_sdk_counter(7, result);
  return result;
}
