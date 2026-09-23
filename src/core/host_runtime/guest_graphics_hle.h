// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <chrono>
#include <stop_token>
#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/hle/call_adapter.h"
namespace Core::HostRuntime {
class GuestGraphics;
// An empty successful lease means cancellation: skip native work and return to
// the backend's pending Cancel path, without manufacturing a syscall fault.
GuestCpu::Result<GuestCpu::ExecutionLease> AcquireGraphicsAdmission(
    GuestCpu::GuestAddressSpace& space, std::stop_token cancel,
    std::chrono::steady_clock::time_point deadline, const std::function<void()>& wait);
// Command streams are byte buffers and may cross adjacent VM allocations.
GuestCpu::Result<GuestCpu::PinnedSpan> AcquireGraphicsCommandBuffer(
    GuestCpu::GuestAddressSpace& space, GuestCpu::GuestRange range, bool writable,
    std::stop_token stop = {});
void InstallGraphicsHandlers(
    std::map<std::string, std::function<GuestCpu::Status(GuestCpu::Hle::HleCallFrame&)>>& handlers,
    std::set<std::string>& gnm, std::set<std::string>& video, GuestCpu::GuestAddressSpace& space,
    std::function<GuestGraphics&()> graphics);
} // namespace Core::HostRuntime
