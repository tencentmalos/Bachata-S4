#pragma once

#include <algorithm>
#include <cstdint>

namespace bachata_pkg {
inline constexpr uint64_t PfsBlockBytes = 0x10000;

constexpr uint64_t PfsBlockCount(uint64_t bytes) {
    return bytes / PfsBlockBytes + (bytes % PfsBlockBytes != 0);
}

constexpr uint64_t PfsWriteBytes(uint64_t file_bytes, uint64_t written) {
    return written >= file_bytes ? 0 : std::min(PfsBlockBytes, file_bytes - written);
}
} // namespace bachata_pkg
