// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <fmt/format.h>
#include "core/libraries/ime/error_dialog.h"
#include "guest_msg_dialog.h"

namespace Core::HostRuntime {
class GuestErrorDialog {
public:
    inline static constexpr std::array<std::string_view, 6> Nids{
        "ekXHb1kDBl0", "t2FvHRXzgqk", "I88KChlynSs", "M2ZF-ClLhgY", "9XAxK2PMwk8", "WWiGuh9XfgQ"};
    GuestErrorDialog() {
        // Desktop ErrorDialog has its own lifecycle and requires no preceding
        // CommonDialogInitialize. Reuse the owned modal renderer with that same
        // independence, without retaining guest addresses in the UI.
        auto common = std::make_shared<GuestCommonDialog>();
        common->Initialize();
        dialog = std::make_shared<GuestMsgDialog>(std::move(common), std::vector<s32>{0});
    }
    static bool IsNid(std::string_view nid) {
        return std::ranges::find(Nids, nid) != Nids.end();
    }
    void Cancel() {
        std::lock_guard lock(mutex);
        stopped = true;
        dialog->Cancel();
    }
    auto Read() const { return dialog->Read(); }
    bool Respond(u64 request) { return dialog->Respond(request, 1); }
    u64 Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
               const std::array<u64, 6>& args) {
        namespace CD = Libraries::CommonDialog;
        try {
            std::lock_guard lock(mutex);
            if (stopped)
                return u32(CD::Error::INVALID_STATE);
            if (nid == "t2FvHRXzgqk" || nid == "WWiGuh9XfgQ")
                return u32(dialog->Read().status);
            if (nid == "I88KChlynSs")
                return dialog->Invoke(space, "lDqxaY1UbEo", {});
            if (nid == "9XAxK2PMwk8")
                return dialog->Invoke(space, "ePw-kqZmelo", {});
            if (nid == "ekXHb1kDBl0")
                return dialog->Invoke(space, "HTrcDKlFKuM", {});
            if (nid != "M2ZF-ClLhgY")
                return u32(CD::Error::NOT_SUPPORTED);
            const auto state = dialog->Read().status;
            if (state != CD::Status::INITIALIZED && state != CD::Status::FINISHED)
                return u32(CD::Error::INVALID_STATE);
            Libraries::ErrorDialog::Param param{};
            if (!args[0] || !space.ReadData({args[0]}, std::as_writable_bytes(std::span{&param, 1})))
                return u32(CD::Error::ARG_NULL);
            if (param.size != sizeof(param))
                return u32(CD::Error::PARAM_INVALID);
            // Desktop displays the code and doesn't require PSN identity. This
            // local presentation user is unrelated to the opaque guest user ID.
            return dialog->OpenLocalMessage(0, fmt::format("An error has occurred.\nCode: {:#X}",
                                                          u32(param.errorCode)));
        } catch (const std::bad_alloc&) {
            return u32(CD::Error::OUT_OF_MEMORY);
        }
    }
private:
    std::mutex mutex;
    bool stopped{};
    std::shared_ptr<GuestMsgDialog> dialog;
};
} // namespace Core::HostRuntime
