// SPDX-License-Identifier: GPL-2.0-or-later
// Reconstructed from eboot+0x154e690..0x154e82c. Measured once per rooftop
// frame; this is the real list-dispatch and retirement body, not a forwarder.
#include "shad_imports.h"
namespace {
template<class T> T read_at(shad_u64 address) {
    T value; __builtin_memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}
template<class T> void write_at(shad_u64 address, T value) {
    __builtin_memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}
void invoke_slot(shad_u64 node, shad_u64 slot) {
    // Reload the vtable before EACH call: a preceding callback may replace it.
    const auto table = read_at<shad_u64>(node);
    const auto method = read_at<shad_u64>(table + slot);
    reinterpret_cast<void (*)(void*)>(method)(reinterpret_cast<void*>(node));
}
void append_pending(shad_u64 queue, shad_u64 offset) {
    auto link = queue + offset + 24;
    auto node = read_at<shad_u64>(link);
    while (node) {
        link = node + 8;
        node = read_at<shad_u64>(link);
    }
    write_at(link, read_at<shad_u64>(queue + offset + 16));
    write_at<shad_u64>(queue + offset + 16, 0);
}
shad_u64 dispatch_calls;
}
extern "C" shad_u64 patch_frame_dispatch(void* object) {
    const auto self = reinterpret_cast<shad_u64>(object);
    for (auto node = read_at<shad_u64>(self + 24); node; node = read_at<shad_u64>(node + 8))
        invoke_slot(node, 32);
    for (auto node = *frame_static_nodes; node; node = read_at<shad_u64>(node + 8))
        invoke_slot(node, 40);
    // The prior global-list callbacks can change the object's head.
    for (auto node = read_at<shad_u64>(self + 24); node; node = read_at<shad_u64>(node + 8)) {
        invoke_slot(node, 40);
        invoke_slot(node, 56);
        invoke_slot(node, 48);
    }
    // Preserve the original single 16-byte unaligned store, then the 4-byte
    // count store. This does not assert atomicity of an unaligned vector write.
    asm volatile("vxorps %%xmm0, %%xmm0, %%xmm0; vmovups %%xmm0, (%0)"
                 : : "r"(self + 24) : "xmm0", "memory");
    write_at<TmntU32>(self + 40, 0);
    for (auto node = *frame_static_nodes; node; node = read_at<shad_u64>(node + 8))
        invoke_slot(node, 48);

    const auto mode = *frame_queue_mode;
    const auto queue = read_at<shad_u64>(self + 48);
    auto* lock = reinterpret_cast<TmntU32*>(queue);
    TmntU32 expected = 0;
    while (!__atomic_compare_exchange_n(lock, &expected, 100, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        expected = 0;
    const auto index = read_at<TmntU32>(queue + 72);
    if (mode != 2) append_pending(queue, shad_u64(index) * 32);
    const TmntU32 opposite_offset = ((~index) & 1) * 32;
    append_pending(queue, opposite_offset);
    // x86 original unlock is MOV, not an extra locked exchange.
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    const auto count = __atomic_add_fetch(&dispatch_calls, 1, __ATOMIC_RELAXED);
    if ((count & 255) == 0) shad_sdk_counter(15, count);
    return opposite_offset;
}
