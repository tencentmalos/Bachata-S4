/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHAD_GUEST_V1_H
#define SHAD_GUEST_V1_H
/* Compiled for the guest x86-64 SysV ABI, never the Android host ABI. */
typedef __UINT64_TYPE__ shad_u64;
typedef __INT64_TYPE__ shad_i64;
typedef __SIZE_TYPE__ shad_size;
#ifdef __cplusplus
extern "C" {
#endif
/* Only these versioned imports cross FEX into the host. No host pointer ABI. */
shad_u64 shad_sdk_query(shad_u64 version); /* v1: bit0 clock, bit1 counters, bit2 logs */
shad_u64 shad_sdk_clock_ns(void); /* host monotonic, includes descheduling */
shad_u64 shad_sdk_counter(shad_u64 declared_id, shad_i64 value);
/* Explicit tagged diagnostic event; tag must be declared by the recipe. */
shad_u64 shad_sdk_log(shad_u64 declared_tag, shad_i64 value);
void *memcpy(void *dst, const void *src, shad_size n);
void *memmove(void *dst, const void *src, shad_size n);
void *memset(void *dst, int value, shad_size n);
int memcmp(const void *a, const void *b, shad_size n);
#ifdef __cplusplus
}
namespace shad {
/* Explicit sampled instrumentation. No clock/HLE cost unless a scope is used.
 * Timings include both clock gateways and scheduling; not guest CPU cycles. */
class TimedScope {
  shad_u64 id, start;

public:
  explicit TimedScope(shad_u64 counter_id)
      : id(counter_id), start(shad_sdk_clock_ns()) {}
  ~TimedScope() {
    shad_sdk_counter(id, (shad_i64)(shad_sdk_clock_ns() - start));
  }
  TimedScope(const TimedScope &) = delete;
  TimedScope &operator=(const TimedScope &) = delete;
};
/* A scalar-only diagnostic event. The host prefixes it with [PATCH_LOG] and
 * resolves the numeric tag through the package declaration. */
inline shad_u64 Log(shad_u64 tag, shad_i64 value) {
  return shad_sdk_log(tag, value);
}
} // namespace shad
#endif
#endif
