// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include "common/types.h"

namespace Core::Crypto {

// AES-128-XTS decryption (IEEE 1619) of whole data units, tweak = unit number (little
// endian), as PFS images use it. Runs on the CPU's AES instructions when present
// (ARMv8 Crypto Extensions, x86 AES-NI); otherwise LibreSSL. The hardware path checks
// itself against LibreSSL once at construction and is not used if they differ.
// Thread-safe: Decrypt keeps no state between calls.
class Aes128Xts {
public:
    enum class Backend { Software, ArmCrypto, AesNi };

    // `key` is the data key followed by the tweak key.
    explicit Aes128Xts(std::span<const u8, 32> key, bool allow_hardware = true);

    // Decrypts data.size() / unit_size consecutive units in place, the first one with
    // tweak `first_unit`. unit_size is a multiple of 16; data.size() of unit_size.
    void Decrypt(u64 first_unit, size_t unit_size, std::span<u8> data) const;

    Backend GetBackend() const {
        return backend;
    }
    static const char* Name(Backend backend);

private:
    void DecryptSoftware(u64 first_unit, size_t unit_size, std::span<u8> data) const;

    Backend backend{Backend::Software};
    std::array<u8, 32> key{};
    // Hardware schedules: tweak-key encryption rounds and data-key decryption rounds
    // (equivalent inverse cipher order), 11 x 16 bytes each.
    alignas(16) std::array<u8, 11 * 16> tweak_rounds{};
    alignas(16) std::array<u8, 11 * 16> data_rounds{};
};

} // namespace Core::Crypto
