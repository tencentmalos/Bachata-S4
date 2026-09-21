// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <unistd.h>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/random/random.h"
#include "core/libraries/random/random_error.h"
namespace Core::HostRuntime {
inline u32 GuestRandom(GuestCpu::GuestAddressSpace& space, u64 address, u64 size) {
    if (size > Libraries::Random::SCE_RANDOM_MAX_SIZE || (size && !address))
        return SCE_RANDOM_ERROR_INVALID;
    if (!size)
        return 0;
    auto pin = space.AcquireDataSpan({GuestCpu::GuestAddress{address}, size}, true);
    if (!pin)
        return SCE_RANDOM_ERROR_INVALID;
    std::array<std::byte, Libraries::Random::SCE_RANDOM_MAX_SIZE> bytes;
    if (::getentropy(bytes.data(), size))
        return SCE_RANDOM_ERROR_FATAL;
    std::memcpy(pin.Value().WritableBytes().data(), bytes.data(), size);
    return 0;
}
} // namespace Core::HostRuntime
