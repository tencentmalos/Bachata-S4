// SPDX-License-Identifier: GPL-2.0-or-later
// Selective x86-64 guest recompilation. This code executes inside FEX, and all
// imported calls below stay in the guest until the original Orbis PLT/HLE gate.
#include "shad_imports.h"

namespace {
template<class T> T load(const TmntSubmitContext* c, shad_size offset) {
    T value;
    __builtin_memcpy(&value, c->storage + offset, sizeof(value));
    return value;
}
template<class T> void store(shad_u64 address, T value) {
    __builtin_memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
// A few sampled reports make actual replacement execution observable without
// clock gateways on every high-frequency command writer. No shared timing slot.
shad_u64 calls[4];
void hit(unsigned i) {
    const auto n = __atomic_add_fetch(&calls[i], 1, __ATOMIC_RELAXED);
    if ((n & 255) == 0) shad_sdk_counter(11 + i, n);
}
}

extern "C" shad_u64 patch_write_label(TmntCommandBuffer* c, TmntU32 event,
                                      shad_u64 label, TmntU32 value, TmntU32 flags) {
    hit(0);
    auto cursor = c->cursor;
    // The original compares the low 32 bits AFTER the unsigned 64-bit shift.
    if (TmntU32((c->end - cursor) >> 2) <= 0x3f) {
        const auto result = c->grow(c, 64, c->grow_context);
        if ((result & 0xff) == 0) return result;
        cursor = c->cursor;
    }
    store<shad_u64>(cursor, 0x68750781c03e1000ULL);
    store<TmntU32>(cursor + 8, TmntU32(label));
    store<TmntU32>(cursor + 12, TmntU32(label >> 32));
    store<TmntU32>(cursor + 16, value);
    store<TmntU32>(cursor + 20, event);
    store<TmntU32>(cursor + 24, flags);
    // Deliberately leave the remaining packet padding untouched, like original.
    c->cursor = cursor + 0x100;
    return c->cursor;
}

extern "C" shad_u64 patch_default_state(TmntCommandBuffer* c) {
    hit(1);
    auto available = (c->end - c->cursor) >> 2;
    if (TmntU32(available) <= 0xff) {
        const auto result = c->grow(c, 256, c->grow_context);
        if ((result & 0xff) == 0) return result;
        available = (c->end - c->cursor) >> 2;
    }
    const auto words = guest_draw_init(reinterpret_cast<TmntU32*>(c->cursor), TmntU32(available));
    const auto bytes = shad_u64(words) << 2;
    // Reload after the call, preserving possible callback-visible cursor changes.
    c->cursor += bytes;
    return bytes;
}

extern "C" shad_u64 patch_submit_packets(shad_u64 device, TmntU32 video,
                                         TmntU32 buffer, TmntU32 mode,
                                         shad_u64 flip_arg, TmntSubmitContext* c) {
    hit(2);
    const auto count = load<TmntU32>(c, 0x1c4);
    // Original reserves 16 slots including the current command buffer. Preserve
    // its behavior through the original trampoline for a shape not yet proved.
    if (count >= 16)
        return original_submit_packets(device, video, buffer, mode, flip_arg, c);
    shad_u64 draw[16], compute[16];
    TmntU32 draw_bytes[16], compute_bytes[16];
    const auto draw_base = load<shad_u64>(c, 0);
    const auto compute_base = load<shad_u64>(c, 0x40);
    for (TmntU32 i = 0; i < count; ++i) {
        const auto record = 0xc4 + shad_size(i) * 16;
        draw[i] = draw_base + shad_u64(load<TmntU32>(c, record)) * 4;
        draw_bytes[i] = load<TmntU32>(c, record + 4) << 2;
        compute_bytes[i] = load<TmntU32>(c, record + 12) << 2;
        compute[i] = compute_bytes[i] ? compute_base + shad_u64(load<TmntU32>(c, record + 8)) * 4 : 0;
    }
    const auto draw_cursor = load<shad_u64>(c, 0x90);
    const auto compute_cursor = load<shad_u64>(c, 0x98);
    draw[count] = draw_cursor;
    draw_bytes[count] = (load<TmntU32>(c, 0x10) - TmntU32(draw_cursor)) & ~TmntU32(3);
    compute_bytes[count] = (load<TmntU32>(c, 0x50) - TmntU32(compute_cursor)) & ~TmntU32(3);
    compute[count] = compute_bytes[count] ? compute_cursor : 0;
    if (device)
        return guest_submit_device(device, count + 1, draw, draw_bytes, compute, compute_bytes,
                                   video, buffer, mode, flip_arg);
    return guest_submit(count + 1, draw, draw_bytes, compute, compute_bytes,
                        video, buffer, mode, flip_arg);
}

extern "C" shad_u64 patch_submit_label(TmntSubmitContext* c, TmntU32 video,
                                       TmntU32 buffer, TmntU32 mode, shad_u64 flip_arg,
                                       TmntU32 event, shad_u64 label, TmntU32 value,
                                       TmntU32 flags) {
    hit(3);
    // Preserve the original sequence, including submission after grow failure.
    patch_write_label(reinterpret_cast<TmntCommandBuffer*>(c), event, label, value, flags);
    return patch_submit_packets(0, video, buffer, mode, flip_arg, c);
}
