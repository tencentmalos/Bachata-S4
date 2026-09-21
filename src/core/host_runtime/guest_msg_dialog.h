// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "guest_common_dialog.h"

namespace Core::HostRuntime {
class GuestMsgDialog final : public std::enable_shared_from_this<GuestMsgDialog> {
public:
    using Status = Libraries::CommonDialog::Status;
    inline static constexpr std::string_view Nids[]{
        "HTrcDKlFKuM", "Lr8ovHH9l6A", "CWVW78Qc3fI", "lDqxaY1UbEo", "b06Hh0DPEaE",
        "Gc5k1qcK4fs", "6H-71OdrpXM", "wTpfglkmv34", "ePw-kqZmelo", "6fIC3XKt2k0"};
    GuestMsgDialog(std::shared_ptr<GuestCommonDialog> common, std::vector<s32> users)
        : common(std::move(common)), users(std::move(users)) {}
    ~GuestMsgDialog();
    static bool IsNid(std::string_view nid);
    u64 Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
               const std::array<u64, 6>& args);
    void Cancel();
    // Local providers use the same owned modal presentation after validating
    // their own ABI. No guest pointers cross this boundary.
    u32 OpenLocalMessage(s32 user, std::string text);
    struct Snapshot {
        Status status{Status::NONE};
        u64 request{};
        s32 user{};
        u32 mode{}, button_type{}, progress{};
        std::string text, first, second;
        bool cancel{}, focus_second{};
    };
    Snapshot Read() const;
    bool Respond(u64 request, u32 action); // 0 cancel, 1 first, 2 second
private:
    u32 Publish(Snapshot next);            // mutex held, validated owned values
    void ReleaseCapture();                 // mutex held
    mutable std::mutex mutex;
    std::shared_ptr<GuestCommonDialog> common;
    std::vector<s32> users;
    Snapshot state;
    u32 result{}, button{};
    bool stopped{}, capture{};
};
void ShowGuestMsgDialog(const std::shared_ptr<GuestMsgDialog>& dialog);
} // namespace Core::HostRuntime
