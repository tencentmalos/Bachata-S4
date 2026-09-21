// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <span>
#include <string_view>
#include "common/types.h"
namespace Core::HostRuntime {
// One persistent FEX owner per host callback worker. No guest pin spans call().
struct GuestCallbackOwner {
    std::function<u64(u64)> begin;
    std::function<u64(u64, std::span<const u64>)> call;
    std::function<void()> cancel;
    std::function<void()> end;
    std::function<void(u64, size_t)> invalidate;
    std::function<void(std::string_view)> fail;
};
}
