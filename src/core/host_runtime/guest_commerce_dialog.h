// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include "guest_msg_dialog.h"

namespace Core::HostRuntime {
// Session-owned local Commerce control. The store has no online provider;
// opening it displays an offline modal and can never authorize a purchase.
class GuestCommerceDialog final {
public:
    inline static constexpr std::string_view Nids[]{
        "NU3ckGHMFXo", "r42bWcQbtZY", "CCbC+lqqvF0", "0aR2aWmQal4", "9ZiLXAGG5rg", "DfSCDRA3EjY",
        "m-I92Ab50W8", "LR5cwFMMCVE", "dsqCVsNM0Zg", "uKTDW8hk-ts", "DHmwsa6S8Tc"};
    GuestCommerceDialog(std::shared_ptr<GuestCommonDialog> common, std::vector<s32> users, u32 sdk);
    ~GuestCommerceDialog();
    static bool IsNid(std::string_view nid);
    u64 Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
               const std::array<u64, 6>& args);
    void Cancel();
    struct Snapshot {
        GuestMsgDialog::Snapshot dialog;
        bool icon_visible{};
        u32 icon_position{}, icon_layout{};
    };
    Snapshot Read() const;
    bool Respond(u64 request); // Only a user cancellation; never a purchase.
private:
    mutable std::mutex mutex;
    std::shared_ptr<GuestMsgDialog> dialog;
    std::vector<s32> users;
    u64 user_data{};
    u32 sdk{};
    bool stopped{}, closed_by_program{}, icon_visible{};
    u32 icon_position{}, icon_layout{};
};
} // namespace Core::HostRuntime
