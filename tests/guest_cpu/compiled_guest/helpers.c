// SPDX-License-Identifier: GPL-2.0-or-later
#include "abi.h"
// Separate C TU, deliberately not LTO/inlined: exercises two spilled SysV args.
guest_u64 guest_c_sum8(guest_u64 a, guest_u64 b, guest_u64 c, guest_u64 d, guest_u64 e, guest_u64 f,
                       guest_u64 g, guest_u64 h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
double guest_c_scale(double value, double scale) {
    return value * scale + 0.25;
}
