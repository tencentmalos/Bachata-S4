// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include "video_core/gpu_reshape_config.h"
int main() {
    unsigned checks{};
    auto check = [&](bool result) { ++checks; if (!result) std::exit(1); };
    using GpuReshape::ParseShaderHashAllowlist;
    check(ParseShaderHashAllowlist("")->empty());
    const auto valid = ParseShaderHashAllowlist("0xFF,0001;ff\tFFFFFFFFFFFFFFFF");
    check(valid && *valid == std::vector<uint64_t>({1, 255, UINT64_MAX}));
    check(ParseShaderHashAllowlist("0")->front() == 0);
    for (const char* invalid : {"garbage", " ", ";,", "0x", "1,invalid", "-1",
                                 "10000000000000000", "123z"})
        check(!ParseShaderHashAllowlist(invalid));
    std::cout << checks << " checks, 0 failures\n";
}
