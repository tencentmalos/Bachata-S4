// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <vector>

#include <openssl/bn.h>
#include <openssl/hmac.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include "common/aes.h"
#include "core/crypto/crypto.h"
#include "core/crypto/keys.h"

namespace Core::Crypto {

namespace {

constexpr size_t AES_BLOCK = 16;
constexpr size_t XTS_SECTOR = 0x1000;

/// Build an RSA key from a keyset's raw big-endian components.
/// Only n and d are needed for a private-key operation; supplying the CRT
/// factors as well lets LibreSSL take the faster path and validates the key.
struct RsaKey {
    RSA* rsa{nullptr};

    template <typename Keyset>
    explicit RsaKey(const Keyset&) : rsa(RSA_new()) {
        if (rsa == nullptr) {
            return;
        }
        BIGNUM* n = BN_bin2bn(Keyset::Modulus, sizeof(Keyset::Modulus), nullptr);
        BIGNUM* e = BN_bin2bn(Keyset::PublicExponent, sizeof(Keyset::PublicExponent), nullptr);
        BIGNUM* d = BN_bin2bn(Keyset::PrivateExponent, sizeof(Keyset::PrivateExponent), nullptr);
        BIGNUM* p = BN_bin2bn(Keyset::Prime1, sizeof(Keyset::Prime1), nullptr);
        BIGNUM* q = BN_bin2bn(Keyset::Prime2, sizeof(Keyset::Prime2), nullptr);
        BIGNUM* dmp1 = BN_bin2bn(Keyset::Exponent1, sizeof(Keyset::Exponent1), nullptr);
        BIGNUM* dmq1 = BN_bin2bn(Keyset::Exponent2, sizeof(Keyset::Exponent2), nullptr);
        BIGNUM* iqmp = BN_bin2bn(Keyset::Coefficient, sizeof(Keyset::Coefficient), nullptr);

        // RSA_set0_* takes ownership of the BIGNUMs on success.
        if (RSA_set0_key(rsa, n, e, d) != 1 || RSA_set0_factors(rsa, p, q) != 1 ||
            RSA_set0_crt_params(rsa, dmp1, dmq1, iqmp) != 1) {
            RSA_free(rsa);
            rsa = nullptr;
        }
    }

    ~RsaKey() {
        if (rsa != nullptr) {
            RSA_free(rsa);
        }
    }

    RsaKey(const RsaKey&) = delete;
    RsaKey& operator=(const RsaKey&) = delete;
};

/// One AES-128 ECB block, used to build the XTS tweak.
void AesEcbEncryptBlock(std::span<const u8, 16> key, const u8 in[16], u8 out[16]) {
    aes::encrypt_ecb(in, AES_BLOCK, key.data(), key.size(), out, AES_BLOCK, /*pads=*/false);
}

void AesEcbDecryptBlock(std::span<const u8, 16> key, const u8 in[16], u8 out[16]) {
    unsigned long padded = 0;
    aes::decrypt_ecb(in, AES_BLOCK, key.data(), key.size(), out, AES_BLOCK, &padded);
}

void XtsXorBlock(u8* x, const u8* a, const u8* b) {
    for (size_t i = 0; i < AES_BLOCK; i++) {
        x[i] = a[i] ^ b[i];
    }
}

/// Multiply the tweak by the primitive element of GF(2^128).
void XtsMult(std::span<u8, 16> tweak) {
    int feedback = 0;
    for (size_t k = 0; k < tweak.size(); k++) {
        const auto tmp = (tweak[k] >> 7) & 1;
        tweak[k] = static_cast<u8>(((tweak[k] << 1) + feedback) & 0xFF);
        feedback = tmp;
    }
    if (feedback != 0) {
        tweak[0] ^= 0x87;
    }
}

} // namespace

void RSA2048Decrypt(std::span<u8, 32> dec_key, std::span<const u8, 256> ciphertext, bool is_dk3) {
    std::array<u8, 256> decrypted{};
    int len = -1;

    if (is_dk3) {
        const RsaKey key{PkgDerivedKey3Keyset{}};
        if (key.rsa != nullptr) {
            len = RSA_private_decrypt(static_cast<int>(ciphertext.size()), ciphertext.data(),
                                      decrypted.data(), key.rsa, RSA_PKCS1_PADDING);
        }
    } else {
        const RsaKey key{FakeKeyset{}};
        if (key.rsa != nullptr) {
            len = RSA_private_decrypt(static_cast<int>(ciphertext.size()), ciphertext.data(),
                                      decrypted.data(), key.rsa, RSA_PKCS1_PADDING);
        }
    }

    // A retail-signed package will not unpad cleanly. Leave dec_key zeroed so
    // the caller's PFSC magic check fails instead of feeding it noise.
    if (len < static_cast<int>(dec_key.size())) {
        std::fill(dec_key.begin(), dec_key.end(), 0);
        return;
    }
    std::memcpy(dec_key.data(), decrypted.data(), dec_key.size());
}

void IvKeyHash256(std::span<const u8, 64> cipher_input, std::span<u8, 32> ivkey_result) {
    SHA256(cipher_input.data(), cipher_input.size(), ivkey_result.data());
}

void AesCbcCfb128Decrypt(std::span<const u8, 32> ivkey, std::span<const u8> ciphertext,
                         std::span<u8> decrypted) {
    std::array<u8, 16> key{};
    std::array<u8, 16> iv{};
    std::memcpy(key.data(), ivkey.data() + 16, key.size());
    std::memcpy(iv.data(), ivkey.data(), iv.size());

    const size_t usable = std::min(ciphertext.size(), decrypted.size()) / AES_BLOCK * AES_BLOCK;
    if (usable == 0) {
        return;
    }
    unsigned long padded = 0;
    aes::decrypt_cbc(ciphertext.data(), static_cast<unsigned long>(usable), key.data(), key.size(),
                     iv.data(), decrypted.data(), static_cast<unsigned long>(usable), &padded);
}

void PfsGenCryptoKey(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                     std::span<u8, 16> data_key, std::span<u8, 16> tweak_key) {
    std::array<u8, 20> d{};
    const u32 index = 1;
    std::memcpy(d.data(), &index, sizeof(index));
    std::memcpy(d.data() + sizeof(index), seed.data(), seed.size());

    std::array<u8, 32> digest{};
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), ekpfs.data(), static_cast<int>(ekpfs.size()), d.data(), d.size(),
         digest.data(), &digest_len);

    // Note the order: the first half is the tweak key, the second the data key.
    std::memcpy(tweak_key.data(), digest.data(), tweak_key.size());
    std::memcpy(data_key.data(), digest.data() + tweak_key.size(), data_key.size());
}

void DecryptPFS(std::span<const u8, 16> data_key, std::span<const u8, 16> tweak_key,
                std::span<const u8> src_image, std::span<u8> dst_image, u64 sector) {
    const size_t size = std::min(src_image.size(), dst_image.size());
    for (size_t i = 0; i + XTS_SECTOR <= size; i += XTS_SECTOR) {
        const u64 current_sector = sector + (i / XTS_SECTOR);

        std::array<u8, 16> tweak{};
        std::array<u8, 16> encrypted_tweak{};
        std::array<u8, 16> xor_buffer{};
        std::memcpy(tweak.data(), &current_sector, sizeof(current_sector));
        AesEcbEncryptBlock(tweak_key, tweak.data(), encrypted_tweak.data());

        for (size_t off = 0; off < XTS_SECTOR; off += AES_BLOCK) {
            XtsXorBlock(xor_buffer.data(), src_image.data() + i + off, encrypted_tweak.data());
            AesEcbDecryptBlock(data_key, xor_buffer.data(), xor_buffer.data());
            XtsXorBlock(dst_image.data() + i + off, xor_buffer.data(), encrypted_tweak.data());
            XtsMult(encrypted_tweak);
        }
    }
}

} // namespace Core::Crypto
