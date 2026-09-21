// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/signin_dialog/signindialog.h"

namespace Core::HostRuntime {
class GuestSigninDialog final {
public:
    static constexpr std::array<std::string_view, 7> Nids{
        "mlYGfmqE3fQ", "JlpJVoRWv7U", "2m077aeC+PA", "Bw31liTFT3A",
        "nqG7rqnYw1U", "M3OkENHcyiU", "LXlmS6PvJdU"};
    explicit GuestSigninDialog(std::vector<s32> users)
        : dialog(std::make_shared<Libraries::SigninDialog::Dialog>(std::move(users))) {}
    ~GuestSigninDialog() {
        Cancel();
    }
    static bool IsNid(std::string_view nid) {
        return std::ranges::find(Nids, nid) != Nids.end();
    }
    void Cancel() {
        dialog->Cancel();
    }
    u64 Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
               const std::array<u64, 6>& args) {
        using namespace Libraries::SigninDialog;
        constexpr u32 bad = static_cast<u32>(Libraries::SigninDialog::Error::InvalidArgument);
        if (nid == "mlYGfmqE3fQ")
            return dialog->Initialize();
        if (nid == "LXlmS6PvJdU")
            return dialog->Terminate();
        if (nid == "M3OkENHcyiU")
            return dialog->Close();
        if (nid == "2m077aeC+PA" || nid == "Bw31liTFT3A")
            return u32(dialog->GetStatus());
        if (nid == "JlpJVoRWv7U") {
            Param param{};
            if (!args[0] || !space.ReadData(GuestCpu::GuestAddress{args[0]},
                                            std::as_writable_bytes(std::span{&param, 1})))
                return bad;
            return dialog->Open(&param);
        }
        if (nid == "nqG7rqnYw1U") {
            if (!args[0])
                return bad;
            auto pin = space.AcquireDataSpan(
                {GuestCpu::GuestAddress{args[0]}, sizeof(Libraries::SigninDialog::Result)}, true);
            if (!pin)
                return bad;
            Libraries::SigninDialog::Result result{};
            const auto error = dialog->GetResult(&result);
            if (error != u32(Libraries::SigninDialog::Error::NotInitialized))
                std::memcpy(pin.Value().WritableBytes().data(), &result, sizeof(result));
            return error;
        }
        return bad;
    }

private:
    std::shared_ptr<Libraries::SigninDialog::Dialog> dialog;
};
} // namespace Core::HostRuntime
