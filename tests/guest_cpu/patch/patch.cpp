#include "shad_imports.h"
extern "C" U64 c_helper(U64);
extern "C" U64 asm_identity(U64);
static U64 count;
static volatile U64 initial = 11;
static U64 (*volatile helper_pointer)(U64) = &c_helper;
extern "C" U64 patch_sum8(U64 a, U64 b, U64 c, U64 d, U64 e, U64 f, U64 g, U64 h) {
    const auto n = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
    if ((n & 1023) == 0)
        shad_sdk_counter(2, n);
    return original_sum8(a, b, c, d, e, f, g, h) + 100;
}
extern "C" U64 patch_rip(U64 x) {
    return original_rip(x) + helper_pointer(initial);
}
extern "C" U64 patch_call(U64 x) {
    return asm_identity(original_call(x)) + 100;
}
extern "C" U64 patch_branch(U64 x) {
    return original_branch(x) + 100;
}
extern "C" double patch_float(double a, double b) {
    return original_float(a, b) + 0.5;
}
extern "C" Triple patch_sret(U64 a) {
    auto s = original_sret(a);
    ++s.c;
    return s;
}
extern "C" U64 probe_fp_sret() {
    // Import slots bind directly to originals; call the patched public entries
    // via explicit test arguments in probe_public below instead.
    const auto s = patch_sret(5);
    return patch_float(1.25, 2.5) == 4.25 && s.a == 5 && s.b == 6 && s.c == 8;
}
extern "C" U64 probe_public(U64 fp, U64 sret) {
    const auto value = reinterpret_cast<double (*)(double, double)>(fp)(1.25, 2.5);
    const auto s = reinterpret_cast<Triple (*)(U64)>(sret)(5);
    return value == 4.25 && s.a == 5 && s.b == 6 && s.c == 8;
}
extern "C" U64 probe_sdk() {
    if (shad_sdk_query(1) != 7 || shad_sdk_query(2) != 0)
        return 0;
    if (shad_sdk_log(1, 0x1234) != 0)
        return 0;
    const auto before = shad_sdk_clock_ns();
    {
        shad::TimedScope scope(1);
        __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
    }
    return shad_sdk_clock_ns() >= before && shad_sdk_counter(999, 0) == U64(-22) &&
           shad_sdk_log(999, 0) == U64(-22);
}
extern "C" U64 read_count() {
    return __atomic_load_n(&count, __ATOMIC_RELAXED);
}
extern "C" U64 probe_bindings() {
    return guest_helper() * ++*guest_count + *guest_bias;
}
extern "C" U64 loop_public(U64 public_sum, U64 n) {
    auto fn = reinterpret_cast<U64 (*)(U64, U64, U64, U64, U64, U64, U64, U64)>(public_sum);
    U64 sum = 0;
    for (U64 i = 0; i < n; ++i)
        sum += fn(1, 2, 3, 4, 5, 6, 7, 8);
    return sum;
}
