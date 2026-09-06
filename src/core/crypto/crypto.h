// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include "common/types.h"

namespace Core::Crypto {

// Primitives needed to unwrap a fake-signed PKG. Originally implemented against
// Crypto++ in src/core/crypto/crypto.cpp; that dependency is gone, so RSA and the
// hashes now come from LibreSSL and AES from the header-only common/aes.h.

/// Raw RSA-2048 private-key operation with PKCS#1 v1.5 unpadding.
/// `is_dk3` selects PkgDerivedKey3Keyset (for DK3) over FakeKeyset (for ekpfs).
/// Writes the leading bytes of the recovered plaintext into `dec_key`.
void RSA2048Decrypt(std::span<u8, 32> dec_key, std::span<const u8, 256> ciphertext, bool is_dk3);

/// SHA-256 over the 64-byte `entry || dk3` blob that seeds the per-entry key.
void IvKeyHash256(std::span<const u8, 64> cipher_input, std::span<u8, 32> ivkey_result);

/// AES-128-CBC over whole blocks. Key is `ivkey[16:32]`, IV is `ivkey[0:16]`.
/// A trailing partial block is left untouched, matching the original loop.
void AesCbcCfb128Decrypt(std::span<const u8, 32> ivkey, std::span<const u8> ciphertext,
                         std::span<u8> decrypted);

/// Derive the PFS data/tweak key pair: HMAC-SHA256(ekpfs) over `u32 index=1 || seed`.
/// The digest's first half is the tweak key, the second half the data key.
void PfsGenCryptoKey(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                     std::span<u8, 16> data_key, std::span<u8, 16> tweak_key);

/// AES-XTS decrypt over 0x1000-byte sectors; the tweak is the LE sector number.
/// `src_image` must be a whole number of sectors.
void DecryptPFS(std::span<const u8, 16> data_key, std::span<const u8, 16> tweak_key,
                std::span<const u8> src_image, std::span<u8> dst_image, u64 sector);

} // namespace Core::Crypto
