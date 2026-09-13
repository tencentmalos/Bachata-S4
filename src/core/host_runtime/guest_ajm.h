// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include "common/types.h"
namespace Core::GuestCpu {
class GuestAddressSpace;
}
namespace Core::HostRuntime {
inline constexpr std::string_view AjmNids[]{
    "dl+4eHSzUu4", "Q3dyFuwGn64", "diXjQNiMu-s", "AxoDrINp4J8", "RbLbuKv8zho",
    "NVDXiUesSbA", "fFFkk0xfGWs", "-qLsfDAywIY", "dmDybN--Fn8", "stlghnic3Jc",
    "ElslOCpOIns", "7jdAXK+2fMo", "bkRHEYG6lEM", "pIpGiaYkHkM", "eDFeTyi+G3Y"};
bool IsAjmNid(std::string_view nid);
// Guest descriptors and buffers never escape into desktop AjmContext. The worker
// only sees owned copies and shared decoders. VM identity is rechecked before a
// short output publication; no pin or VM gate spans decode or BatchWait.
class GuestAjm {
public:
    GuestAjm(GuestCpu::GuestAddressSpace& space, std::recursive_mutex& vm);
    ~GuestAjm();
    u64 Dispatch(std::string_view nid, const std::array<u64, 10>& args,
                 std::stop_token cancel = {});
    void RequestStop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
