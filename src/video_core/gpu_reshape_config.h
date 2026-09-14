// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace GpuReshape {
// Empty means no restriction. Any malformed nonempty input must fail closed;
// silently dropping invalid hashes would accidentally instrument every shader.
inline std::optional<std::vector<uint64_t>> ParseShaderHashAllowlist(std::string_view text) {
    std::vector<uint64_t> hashes;
    if (text.empty()) return hashes;
    constexpr std::string_view separators{",; \t\r\n"};
    while (!text.empty()) {
        const auto begin = text.find_first_not_of(separators);
        if (begin == std::string_view::npos) break;
        text.remove_prefix(begin);
        const auto end = text.find_first_of(separators);
        auto token = text.substr(0, end);
        if (token.starts_with("0x") || token.starts_with("0X")) token.remove_prefix(2);
        uint64_t hash{};
        const auto [ptr, error] = std::from_chars(token.data(), token.data() + token.size(), hash, 16);
        if (token.empty() || error != std::errc{} || ptr != token.data() + token.size())
            return std::nullopt;
        hashes.push_back(hash);
        if (end == std::string_view::npos) break;
        text.remove_prefix(end);
    }
    if (hashes.empty()) return std::nullopt;
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    return hashes;
}
} // namespace GpuReshape
