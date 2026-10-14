// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/crypto/aes_xts.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <openssl/evp.h>
#include "common/logging/log.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#define AES_XTS_ARM 1
#include <arm_neon.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#endif
#if defined(__clang__) || defined(__GNUC__)
#define ARM_AES_TARGET __attribute__((target("aes")))
#else
#define ARM_AES_TARGET
#endif
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define AES_XTS_X86 1
#include <emmintrin.h>
#include <wmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(__clang__) || defined(__GNUC__)
#define X86_AES_TARGET __attribute__((target("aes,sse2")))
#else
#define X86_AES_TARGET
#endif
#endif

namespace Core::Crypto {
namespace {

constexpr u8 kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

// AES-128 key schedule (FIPS-197 5.2) on a little-endian host: word i holds bytes
// 4i..4i+3 in order, so RotWord is a right rotation by 8 and Rcon goes to the low byte.
// `sub` is SubWord, computed with the CPU's AES instruction.
template <class SubWord>
void ExpandKey(const u8* key, u8* rounds, SubWord sub) {
    u32 w[44];
    std::memcpy(w, key, 16);
    for (int i = 4; i < 44; ++i) {
        u32 t = w[i - 1];
        if (i % 4 == 0) {
            t = sub(t);
            t = (t >> 8) | (t << 24);
            t ^= kRcon[i / 4 - 1];
        }
        w[i] = w[i - 4] ^ t;
    }
    std::memcpy(rounds, w, sizeof(w));
}

void UnitTweak(u64 unit, u64& lo, u64& hi) {
    lo = unit;
    hi = 0;
}
// Multiplication by x in GF(2^128), XTS little-endian convention.
inline void NextTweak(u64& lo, u64& hi) {
    const u64 carry = hi >> 63;
    hi = (hi << 1) | (lo >> 63);
    lo = (lo << 1) ^ (0x87 & (0 - carry));
}

#ifdef AES_XTS_ARM
bool ArmHasAes() {
#if defined(__linux__) || defined(__ANDROID__)
    return (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
#elif defined(__APPLE__)
    return true; // every Apple arm64 CPU implements FEAT_AES
#else
    return false;
#endif
}

ARM_AES_TARGET u32 SubWordArm(u32 w) {
    // AESE with a zero round key is SubBytes(ShiftRows(state)); with four equal
    // columns ShiftRows changes nothing, so lane 0 is SubWord(w).
    const uint8x16_t s = vaeseq_u8(vreinterpretq_u8_u32(vdupq_n_u32(w)), vdupq_n_u8(0));
    return vgetq_lane_u32(vreinterpretq_u32_u8(s), 0);
}

ARM_AES_TARGET void SetupArm(const u8* key, u8* tweak_rounds, u8* data_rounds) {
    alignas(16) u8 data[176];
    ExpandKey(key, data, SubWordArm);
    ExpandKey(key + 16, tweak_rounds, SubWordArm);
    // Equivalent inverse cipher: round 10 key, InvMixColumns of rounds 9..1, round 0 key.
    vst1q_u8(data_rounds, vld1q_u8(data + 160));
    for (int i = 1; i < 10; ++i)
        vst1q_u8(data_rounds + 16 * i, vaesimcq_u8(vld1q_u8(data + 16 * (10 - i))));
    vst1q_u8(data_rounds + 160, vld1q_u8(data));
}

inline uint8x16_t TweakVector(u64 lo, u64 hi) {
    return vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(lo), vcreate_u64(hi)));
}

ARM_AES_TARGET void DecryptArm(const u8* tweak_rounds, const u8* data_rounds, u64 unit,
                               size_t unit_size, u8* data, size_t size) {
    uint8x16_t ek[11], dk[11];
    for (int i = 0; i < 11; ++i) {
        ek[i] = vld1q_u8(tweak_rounds + 16 * i);
        dk[i] = vld1q_u8(data_rounds + 16 * i);
    }
    for (size_t offset = 0; offset < size; offset += unit_size, ++unit) {
        u64 lo, hi;
        UnitTweak(unit, lo, hi);
        uint8x16_t t = TweakVector(lo, hi);
        for (int i = 0; i < 9; ++i)
            t = vaesmcq_u8(vaeseq_u8(t, ek[i]));
        t = veorq_u8(vaeseq_u8(t, ek[9]), ek[10]);
        lo = vgetq_lane_u64(vreinterpretq_u64_u8(t), 0);
        hi = vgetq_lane_u64(vreinterpretq_u64_u8(t), 1);
        u8* p = data + offset;
        size_t b = 0;
        // Four blocks at a time keep the AES units busy.
        for (; b + 64 <= unit_size; b += 64) {
            uint8x16_t t0 = TweakVector(lo, hi);
            NextTweak(lo, hi);
            uint8x16_t t1 = TweakVector(lo, hi);
            NextTweak(lo, hi);
            uint8x16_t t2 = TweakVector(lo, hi);
            NextTweak(lo, hi);
            uint8x16_t t3 = TweakVector(lo, hi);
            NextTweak(lo, hi);
            uint8x16_t c0 = veorq_u8(vld1q_u8(p + b), t0);
            uint8x16_t c1 = veorq_u8(vld1q_u8(p + b + 16), t1);
            uint8x16_t c2 = veorq_u8(vld1q_u8(p + b + 32), t2);
            uint8x16_t c3 = veorq_u8(vld1q_u8(p + b + 48), t3);
            for (int i = 0; i < 9; ++i) {
                c0 = vaesimcq_u8(vaesdq_u8(c0, dk[i]));
                c1 = vaesimcq_u8(vaesdq_u8(c1, dk[i]));
                c2 = vaesimcq_u8(vaesdq_u8(c2, dk[i]));
                c3 = vaesimcq_u8(vaesdq_u8(c3, dk[i]));
            }
            c0 = veorq_u8(vaesdq_u8(c0, dk[9]), dk[10]);
            c1 = veorq_u8(vaesdq_u8(c1, dk[9]), dk[10]);
            c2 = veorq_u8(vaesdq_u8(c2, dk[9]), dk[10]);
            c3 = veorq_u8(vaesdq_u8(c3, dk[9]), dk[10]);
            vst1q_u8(p + b, veorq_u8(c0, t0));
            vst1q_u8(p + b + 16, veorq_u8(c1, t1));
            vst1q_u8(p + b + 32, veorq_u8(c2, t2));
            vst1q_u8(p + b + 48, veorq_u8(c3, t3));
        }
        for (; b < unit_size; b += 16) {
            const uint8x16_t tb = TweakVector(lo, hi);
            NextTweak(lo, hi);
            uint8x16_t c = veorq_u8(vld1q_u8(p + b), tb);
            for (int i = 0; i < 9; ++i)
                c = vaesimcq_u8(vaesdq_u8(c, dk[i]));
            c = veorq_u8(vaesdq_u8(c, dk[9]), dk[10]);
            vst1q_u8(p + b, veorq_u8(c, tb));
        }
    }
}
#endif

#ifdef AES_XTS_X86
bool X86HasAes() {
#if defined(_MSC_VER)
    int info[4]{};
    __cpuid(info, 1);
    return (info[2] & (1 << 25)) != 0;
#else
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return false;
    return (ecx & (1u << 25)) != 0;
#endif
}

X86_AES_TARGET u32 SubWordX86(u32 w) {
    // AESKEYGENASSIST's first result dword is SubWord of the second source dword.
    return u32(_mm_cvtsi128_si32(_mm_aeskeygenassist_si128(_mm_set1_epi32(int(w)), 0)));
}

X86_AES_TARGET void SetupX86(const u8* key, u8* tweak_rounds, u8* data_rounds) {
    alignas(16) u8 data[176];
    ExpandKey(key, data, SubWordX86);
    ExpandKey(key + 16, tweak_rounds, SubWordX86);
    const auto load = [&](int r) {
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * r));
    };
    _mm_storeu_si128(reinterpret_cast<__m128i*>(data_rounds), load(10));
    for (int i = 1; i < 10; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(data_rounds + 16 * i),
                         _mm_aesimc_si128(load(10 - i)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(data_rounds + 160), load(0));
}

X86_AES_TARGET void DecryptX86(const u8* tweak_rounds, const u8* data_rounds, u64 unit,
                               size_t unit_size, u8* data, size_t size) {
    __m128i ek[11], dk[11];
    for (int i = 0; i < 11; ++i) {
        ek[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tweak_rounds + 16 * i));
        dk[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_rounds + 16 * i));
    }
    for (size_t offset = 0; offset < size; offset += unit_size, ++unit) {
        __m128i t = _mm_xor_si128(_mm_set_epi64x(0, static_cast<long long>(unit)), ek[0]);
        for (int i = 1; i < 10; ++i)
            t = _mm_aesenc_si128(t, ek[i]);
        t = _mm_aesenclast_si128(t, ek[10]);
        alignas(16) u64 halves[2];
        _mm_store_si128(reinterpret_cast<__m128i*>(halves), t);
        u64 lo = halves[0], hi = halves[1];
        u8* p = data + offset;
        for (size_t b = 0; b < unit_size; b += 16) {
            const __m128i tb = _mm_set_epi64x(static_cast<long long>(hi), static_cast<long long>(lo));
            NextTweak(lo, hi);
            __m128i c = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + b)), tb);
            c = _mm_xor_si128(c, dk[0]);
            for (int i = 1; i < 10; ++i)
                c = _mm_aesdec_si128(c, dk[i]);
            c = _mm_aesdeclast_si128(c, dk[10]);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p + b), _mm_xor_si128(c, tb));
        }
    }
}
#endif

} // namespace

Aes128Xts::Aes128Xts(std::span<const u8, 32> key_bytes, bool allow_hardware) {
    std::memcpy(key.data(), key_bytes.data(), key.size());
    if (!allow_hardware)
        return;
#ifdef AES_XTS_ARM
    if (ArmHasAes()) {
        SetupArm(key.data(), tweak_rounds.data(), data_rounds.data());
        backend = Backend::ArmCrypto;
    }
#endif
#ifdef AES_XTS_X86
    if (X86HasAes()) {
        SetupX86(key.data(), tweak_rounds.data(), data_rounds.data());
        backend = Backend::AesNi;
    }
#endif
    if (backend == Backend::Software)
        return;
    // Self-check: two units of fixed pseudo-random ciphertext through both paths.
    std::vector<u8> hardware(1024), software;
    u32 x = 0x9e3779b9u;
    for (auto& b : hardware) {
        x = x * 1664525u + 1013904223u;
        b = u8(x >> 24);
    }
    software = hardware;
    Decrypt(0x123456789abcdefull, 512, hardware);
    DecryptSoftware(0x123456789abcdefull, 512, software);
    if (hardware != software) {
        LOG_WARNING(Core, "AES-XTS {} result differs from LibreSSL; using LibreSSL",
                    Name(backend));
        backend = Backend::Software;
    }
}

void Aes128Xts::Decrypt(u64 first_unit, size_t unit_size, std::span<u8> data) const {
    if (!unit_size || unit_size % 16 || data.size() % unit_size)
        throw std::invalid_argument("AES-XTS: data is not whole units of whole blocks");
    switch (backend) {
#ifdef AES_XTS_ARM
    case Backend::ArmCrypto:
        DecryptArm(tweak_rounds.data(), data_rounds.data(), first_unit, unit_size, data.data(),
                   data.size());
        return;
#endif
#ifdef AES_XTS_X86
    case Backend::AesNi:
        DecryptX86(tweak_rounds.data(), data_rounds.data(), first_unit, unit_size, data.data(),
                   data.size());
        return;
#endif
    default:
        DecryptSoftware(first_unit, unit_size, data);
        return;
    }
}

void Aes128Xts::DecryptSoftware(u64 first_unit, size_t unit_size, std::span<u8> data) const {
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx{EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free};
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_xts(), nullptr, key.data(), nullptr) != 1)
        throw std::runtime_error("AES-XTS: cipher setup failed");
    std::vector<u8> plain(unit_size);
    for (size_t offset = 0; offset < data.size(); offset += unit_size, ++first_unit) {
        std::array<u8, 16> iv{};
        for (size_t j = 0; j < 8; ++j)
            iv[j] = u8(first_unit >> (j * 8));
        int written = 0;
        // A null key keeps the schedule and only replaces the tweak.
        if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, nullptr, iv.data()) != 1 ||
            EVP_DecryptUpdate(ctx.get(), plain.data(), &written, data.data() + offset,
                              static_cast<int>(unit_size)) != 1 ||
            written != static_cast<int>(unit_size))
            throw std::runtime_error("AES-XTS: decryption failed");
        std::memcpy(data.data() + offset, plain.data(), unit_size);
    }
}

const char* Aes128Xts::Name(Backend backend) {
    switch (backend) {
    case Backend::ArmCrypto:
        return "arm-crypto";
    case Backend::AesNi:
        return "aes-ni";
    default:
        return "libressl";
    }
}

} // namespace Core::Crypto
