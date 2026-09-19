// SPDX-License-Identifier: GPL-2.0-or-later
#include "shad_entry.h"
// 只观察入口；AVX adapter 恢复寄存器并执行原函数，不跳过任何初始化。
namespace {
shad_u64 counts[11];
// 由 SelectDevice / ReadDeviceNames 原机器码恢复的 Unity 字符串和数组布局。
struct UnityString { const unsigned char* heap; unsigned char small[16]; shad_u64 size; unsigned int allocator; unsigned int padding; };
struct UnityStringVector { shad_u64 allocator; const UnityString* begin; const UnityString* end; const UnityString* capacity; };
static_assert(sizeof(UnityString) == 40);
void LogNames(const ShadGuestEntryContext* entry) {
    const auto* list = reinterpret_cast<const UnityStringVector*>(entry->gpr->rsi);
    const auto first = reinterpret_cast<shad_u64>(list->begin);
    const auto last = reinterpret_cast<shad_u64>(list->end);
    if (last < first || (last-first) % 40 || (last-first) / 40 > 64) return;
    const auto size = (last-first) / 40;
    shad_sdk_log(45, size);
    for (shad_u64 i=0; i<size && i<2; ++i) {
        const auto& text = list->begin[i];
        const auto* bytes = text.heap ? text.heap : text.small;
        shad_u64 lo=0, hi=0;
        for (shad_u64 j=0; j<text.size && j<16; ++j) {
            if (j<8) lo |= shad_u64(bytes[j]) << (8*j);
            else hi |= shad_u64(bytes[j]) << (8*(j-8));
        }
        shad_sdk_log(46+i*3, text.size);
        shad_sdk_log(47+i*3, lo);
        shad_sdk_log(48+i*3, hi);
    }
    const auto* manager = reinterpret_cast<const unsigned char*>(entry->gpr->rdi);
    shad_sdk_log(52, shad_u64(manager[0]) | (shad_u64(manager[1])<<8) |
        (shad_u64(manager[80])<<16) | (shad_u64(manager[112])<<24));
}
}
extern "C" void observe_UnityVR_RegisterProviders(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[0], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(1, n);
        shad_sdk_log(1, n);
        shad_sdk_log(2, entry->gpr->rdi);
        shad_sdk_log(3, entry->gpr->rsi);
        shad_sdk_log(4, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityVR_SelectDevice(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[1], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(2, n);
        shad_sdk_log(5, n);
        shad_sdk_log(6, entry->gpr->rdi);
        shad_sdk_log(7, entry->gpr->rsi);
        shad_sdk_log(8, entry->gpr->rdx);
        LogNames(entry);
    }
}
extern "C" void observe_UnityVR_ReadDeviceNames(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[2], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(3, n);
        shad_sdk_log(9, n);
        shad_sdk_log(10, entry->gpr->rdi);
        shad_sdk_log(11, entry->gpr->rsi);
        shad_sdk_log(12, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityVR_InitializeSelectedDevice(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[3], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(4, n);
        shad_sdk_log(13, n);
        shad_sdk_log(14, entry->gpr->rdi);
        shad_sdk_log(15, entry->gpr->rsi);
        shad_sdk_log(16, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_Initialize(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[4], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(5, n);
        shad_sdk_log(17, n);
        shad_sdk_log(18, entry->gpr->rdi);
        shad_sdk_log(19, entry->gpr->rsi);
        shad_sdk_log(20, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_RegisterManagedCalls(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[5], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(6, n);
        shad_sdk_log(21, n);
        shad_sdk_log(22, entry->gpr->rdi);
        shad_sdk_log(23, entry->gpr->rsi);
        shad_sdk_log(24, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_GetHmdMount(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[6], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(7, n);
        shad_sdk_log(25, n);
        shad_sdk_log(26, entry->gpr->rdi);
        shad_sdk_log(27, entry->gpr->rsi);
        shad_sdk_log(28, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_GetOwnerUserId(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[7], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(8, n);
        shad_sdk_log(29, n);
        shad_sdk_log(30, entry->gpr->rdi);
        shad_sdk_log(31, entry->gpr->rsi);
        shad_sdk_log(32, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_SetupOpenAsync(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[8], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(9, n);
        shad_sdk_log(33, n);
        shad_sdk_log(34, entry->gpr->rdi);
        shad_sdk_log(35, entry->gpr->rsi);
        shad_sdk_log(36, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_SetupUpdate(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[9], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(10, n);
        shad_sdk_log(37, n);
        shad_sdk_log(38, entry->gpr->rdi);
        shad_sdk_log(39, entry->gpr->rsi);
        shad_sdk_log(40, entry->gpr->rdx);
    }
}
extern "C" void observe_UnityPSVR_SetupGetResult(const ShadGuestEntryContext* entry) {
    const auto n = __atomic_add_fetch(&counts[10], 1, __ATOMIC_RELAXED);
    if (n <= 4 || (n & 1023) == 0) {
        shad_sdk_counter(11, n);
        shad_sdk_log(41, n);
        shad_sdk_log(42, entry->gpr->rdi);
        shad_sdk_log(43, entry->gpr->rsi);
        shad_sdk_log(44, entry->gpr->rdx);
    }
}
