// SPDX-License-Identifier: GPL-2.0-or-later
#include "shad_imports.h"
// InvokeGuest intentionally has eight API argument slots. This guest caller
// exercises the real nine-argument SysV ABI, including three stack arguments.
extern "C" shad_u64 replay_submit_label(shad_u64 target, TmntSubmitContext* c, shad_u64 label) {
    const auto call = reinterpret_cast<decltype(&patch_submit_label)>(target);
    return call(c, 0x80123456, 0x98765432, 0x12345678, 0x9988776655443322, 4, label, 2, 0x38);
}
