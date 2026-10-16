// SPDX-License-Identifier: GPL-2.0-or-later
// Reconstructed semantics of Tetris CUSA13427, Release-4.26. Not Epic/Sony source.
// See docs/gnm/tetris-ue426-gnmrhi-readback.md for evidence and unresolved boundaries.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace Tetris426 {

using GuestAddress = std::uint64_t;

struct NativeTexture {
    virtual ~NativeTexture() = default;
    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
    // SDK 0x3f3ae20: handles format/tile-dependent address bits. Do not simply
    // substitute raw descriptor DWORD0 for this operation on every format.
    virtual std::uint32_t BaseAddress256() const = 0;
};

struct MappedSurface {
    GuestAddress data;
    std::uint32_t width;
    std::uint32_t height;
};

// Native FGnmDynamicRHI::RHIMapStagingSurface candidate, eboot 0xfa13d0.
// Fence readiness / RHI-thread flushing belong to the outer wrapper (0x17c8110).
// The returned width is the native texture width, not a computed byte row pitch.
inline MappedSurface RHIMapStagingSurface(const NativeTexture& texture) {
    return {GuestAddress(texture.BaseAddress256()) << 8, texture.Width(), texture.Height()};
}

// eboot 0xfa1430 is an actual empty function, not an unimplemented placeholder.
inline void RHIUnmapStagingSurface(const NativeTexture&) {}

struct Backend {
    virtual ~Backend() = default;
    virtual std::uint32_t ReadLabel32(GuestAddress address) const = 0;
    virtual void RecordEop(const std::array<std::uint32_t, 6>& words) = 0;
};

struct FGnmGPUFence {
    GuestAddress label_address = 0; // native +0x20 (inside its allocation owner)
    std::uint32_t expected = 0;    // native +0x40
    bool armed = false;           // native +0x44
    std::uint32_t pending_writes = 0; // FRHIGPUFence +0x14; conceptual, single-thread model

    // 0xf8c380: do not clear the label or reset the sequence.
    void Clear() { armed = false; }
    // 0xf8c390: equality, not greater-or-equal and not merely a nonzero label.
    bool Poll(const Backend& backend) const {
        return armed && backend.ReadLabel32(label_address) == expected;
    }
};

// FGnmCommandContext candidate 0xf991a0 -> SDK 0x3f37180.
// Valid caller owns a 4-byte aligned label below 2^40. The SDK emitter masks the
// address; this reference rejects invalid model inputs instead of hiding them.
inline void RHIWriteGPUFence(Backend& backend, FGnmGPUFence& fence) {
    if (!fence.label_address || (fence.label_address & 3) || fence.label_address >= (1ull << 40)) {
        throw std::invalid_argument("invalid GNM fence label address");
    }
    ++fence.expected;
    backend.RecordEop({{0xc0044700u, 0x04000528u,
                       std::uint32_t(fence.label_address),
                       std::uint32_t(fence.label_address >> 32) | 0x20000000u,
                       fence.expected, 0u}});
    fence.armed = true;
}

// These are explicit integration boundaries, not reconstructed full UE classes.
// A backend must implement staging allocation, copy/transition recording, fence
// submission, actual GPU completion, CPU visibility, and object lifetime.
struct Texture { virtual ~Texture() = default; };
struct TextureReadback {
    virtual ~TextureReadback() = default;
    virtual const FGnmGPUFence* Fence() const = 0;
    virtual const void* Lock16() = 0; // first float4; 0x17c9a60 / MapStagingSurface
    virtual void Unlock() = 0;
};
struct ReadbackBackend : Backend {
    virtual TextureReadback& GetOrCreateReadback(unsigned slot) = 0;
    virtual void EnqueueCopy(TextureReadback&, Texture&) = 0;
};

// Texture variant at 0x2170ba0. The separate buffer variant 0x21710f0 is NOT
// implemented by this class. Pointer ownership is supplied by ReadbackBackend.
struct FEyeAdaptationTextureHistory {
    unsigned current = 0; // native +0; invariant 0..2
    float last_exposure = 0; // native +4; FSceneViewState +0x424
    float last_auxiliary = 0; // native +8; reads texel component Z, name unresolved
    std::array<Texture*, 3> textures{{nullptr, nullptr, nullptr}}; // native +0x10
    std::array<TextureReadback*, 3> readbacks{{nullptr, nullptr, nullptr}}; // native +0x28

    void Advance(ReadbackBackend& backend, bool readback_enabled, bool readback_suppressed) {
        if (current >= 3) throw std::logic_error("invalid exposure history index");
        const unsigned next = (current + 1) % 3;
        if (readback_enabled && textures[current] && !readback_suppressed) {
            // 0x17f7730 registers the current texture with RDG before this check.
            // Registration itself is an integration responsibility here.
            TextureReadback* old = readbacks[next];
            const FGnmGPUFence* fence = old ? old->Fence() : nullptr;
            if (old && (!fence || (fence->pending_writes == 0 && fence->Poll(backend)))) {
                if (const void* mapped = old->Lock16()) {
                    std::memcpy(&last_exposure, mapped, sizeof(float));
                    std::memcpy(&last_auxiliary,
                                static_cast<const unsigned char*>(mapped) + 8, sizeof(float));
                    old->Unlock();
                }
            }
            if (!readbacks[current]) {
                readbacks[current] = &backend.GetOrCreateReadback(current);
            }
            backend.EnqueueCopy(*readbacks[current], *textures[current]);
        }
        current = next; // advances even when readback is disabled or source is absent
    }
};

struct ViewExposure { float pre_exposure; float one_over_pre_exposure; };

// Ordinary enabled branch of 0x2171340 and packing at 0x22e8bbc..0x22e8bda.
// Other view/show-flag/manual-exposure branches are intentionally outside this subset.
// No finite clamp: a tiny positive readback really passes the >0 test and can
// overflow the reciprocal. Adding a clamp would conceal the observed emulator bug.
inline ViewExposure PackOrdinaryViewExposure(float last_exposure, float positive_override = 0) {
    float pre = 1.0f;
    if (positive_override > 0) pre = positive_override;
    else if (last_exposure > 0) pre = last_exposure;
    return {pre, 1.0f / pre};
}

} // namespace Tetris426
