// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstring>
#include "common/logging/log.h"
#include "core/libraries/np/np_commerce/np_commerce.h"
#include "core/libraries/np/np_commerce/np_commerce_ui.h"
#include "core/libraries/system/userservice.h"
#include "guest_commerce_dialog.h"

namespace Core::HostRuntime {
namespace CD = Libraries::CommonDialog;
namespace NC = Libraries::Np::NpCommerce;
namespace {
constexpr u32 Code(CD::Error error) {
    return static_cast<u32>(error);
}
template <class T>
bool ReadValue(GuestCpu::GuestAddressSpace& space, u64 address, T& value) {
    return address && bool(space.ReadData({address}, std::as_writable_bytes(std::span{&value, 1})));
}
template <class T>
bool Zero(const T& bytes) {
    return std::ranges::all_of(bytes, [](auto value) { return value == 0; });
}
// The firmware keeps a 42-byte field per target. Copy the same bounded prefix,
// without dereferencing a guest pointer inside the native UI.
bool Target(GuestCpu::GuestAddressSpace& space, u64 address, std::string& output) {
    if (!address || address > UINT64_MAX - 41)
        return false;
    for (size_t i = 0; i < 41; ++i) {
        char c{};
        if (!ReadValue(space, address + i, c))
            return false;
        if (!c)
            return true;
        output += c;
    }
    return true;
}
} // namespace
GuestCommerceDialog::GuestCommerceDialog(std::shared_ptr<GuestCommonDialog> common,
                                         std::vector<s32> users, u32 sdk)
    : dialog(std::make_shared<GuestMsgDialog>(std::move(common), users)), users(std::move(users)),
      sdk(sdk) {}
GuestCommerceDialog::~GuestCommerceDialog() {
    Cancel();
}
bool GuestCommerceDialog::IsNid(std::string_view nid) {
    return std::ranges::find(Nids, nid) != std::end(Nids);
}
void GuestCommerceDialog::Cancel() {
    std::lock_guard lock(mutex);
    if (stopped)
        return;
    stopped = true;
    dialog->Cancel();
    if (icon_visible)
        NC::PsStoreIconLayer::Instance().Hide();
    icon_visible = false;
}
GuestCommerceDialog::Snapshot GuestCommerceDialog::Read() const {
    std::lock_guard lock(mutex);
    return {dialog->Read(), icon_visible, icon_position, icon_layout};
}
bool GuestCommerceDialog::Respond(u64 request) {
    std::lock_guard lock(mutex);
    return !stopped && dialog->Respond(request, 0);
}
u64 GuestCommerceDialog::Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                                const std::array<u64, 6>& args) {
    try {
        std::lock_guard lock(mutex);
        if (stopped)
            return Code(CD::Error::INVALID_STATE);
        const auto state = dialog->Read().status;
        if (nid == "DHmwsa6S8Tc") {
            icon_position = u32(args[0]) == 1 ? 1 : u32(args[0]) == 2 ? 2 : 0;
            NC::PsStoreIconLayer::Instance().SetLayout(
                static_cast<NC::PsStoreIconLayout>(icon_layout));
            NC::PsStoreIconLayer::Instance().Show(static_cast<NC::PsStoreIconPos>(icon_position));
            icon_visible = true;
            return 0;
        }
        if (nid == "dsqCVsNM0Zg") {
            NC::PsStoreIconLayer::Instance().Hide();
            icon_visible = false;
            return 0;
        }
        if (nid == "uKTDW8hk-ts") {
            // Firmware maps unknown layouts to the default; reuse the desktop renderer.
            icon_layout = u32(args[0]) == 1 ? 1 : u32(args[0]) == 2 ? 2 : 0;
            NC::PsStoreIconLayer::Instance().SetLayout(
                static_cast<NC::PsStoreIconLayout>(icon_layout));
            return 0;
        }
        if (nid == "0aR2aWmQal4" || nid == "9ZiLXAGG5rg")
            return dialog->Invoke(space, "lDqxaY1UbEo", {});
        if (nid == "CCbC+lqqvF0" || nid == "LR5cwFMMCVE")
            return u32(state);
        if (nid == "m-I92Ab50W8")
            return dialog->Invoke(space, "ePw-kqZmelo", {});
        if (nid == "NU3ckGHMFXo") {
            if (state == CD::Status::NONE)
                return Code(CD::Error::NOT_INITIALIZED);
            if (state == CD::Status::FINISHED)
                return Code(CD::Error::ALREADY_CLOSE);
            const auto result = dialog->Invoke(space, "HTrcDKlFKuM", {});
            if (!result)
                closed_by_program = true;
            return result;
        }
        if (nid == "r42bWcQbtZY") {
            if (!args[0])
                return Code(CD::Error::ARG_NULL);
            if (state == CD::Status::NONE)
                return Code(CD::Error::NOT_INITIALIZED);
            if (state != CD::Status::FINISHED)
                return Code(CD::Error::NOT_FINISHED);
            if (sdk < 0x01700000)
                return Code(CD::Error::NOT_SUPPORTED);
            auto output =
                space.AcquireDataSpan({{args[0]}, sizeof(NC::OrbisNpCommerceDialogResult)}, true);
            if (!output)
                return Code(CD::Error::ARG_NULL);
            NC::OrbisNpCommerceDialogResult result{};
            // Firmware 11.00 +0x1086 distinguishes API Close from user cancel
            // for SDK >=2.00. Neither result grants online authorization.
            result.result = closed_by_program && sdk >= 0x02000000 ? 0 : 1;
            result.userData = reinterpret_cast<void*>(user_data);
            std::memcpy(output.Value().WritableBytes().data(), &result, sizeof(result));
            return result.result;
        }
        if (nid != "DfSCDRA3EjY")
            return Code(CD::Error::NOT_SUPPORTED);
        if (state == CD::Status::NONE)
            return Code(CD::Error::NOT_INITIALIZED);
        if (state != CD::Status::INITIALIZED && state != CD::Status::FINISHED)
            return Code(CD::Error::INVALID_STATE);
        if (sdk < 0x01700000)
            return Code(CD::Error::NOT_SUPPORTED);
        NC::OrbisNpCommerceDialogParam param{};
        if (!ReadValue(space, args[0], param))
            return Code(CD::Error::ARG_NULL);
        if (param.size != sizeof(param) || param.baseParam.size != sizeof(param.baseParam) ||
            param.baseParam.magic != u32(args[0] + 0xc0d1a109) || !Zero(param.baseParam.reserved) ||
            !Zero(param.reserved) || param.serviceLabel > 7 ||
            std::ranges::find(users, param.userId) == users.end())
            return Code(CD::Error::PARAM_INVALID);
        if (param.mode == 1000 || param.mode == 1001)
            return Code(CD::Error::NOT_SUPPORTED);
        if (param.mode < 0 || param.mode > 5)
            return Code(CD::Error::PARAM_INVALID);
        const auto count = param.numTargets;
        if (count > 10 || ((param.mode == 0 || param.mode == 2) && count > 1) ||
            (param.mode == 1 && count != 1) || (param.mode == 3 && count == 0) ||
            (param.mode == 5 &&
             (count || (sdk >= 0x03500000 ? param.features != 1
                                          : param.features < 1 || param.features > 3))) ||
            (count && !param.targets) || (!count && param.mode != 5 && param.targets))
            return Code(CD::Error::PARAM_INVALID);
        std::string message = "PlayStation Store is unavailable while offline.\nNo purchase or "
                              "subscription was made.";
        const u64 targets = reinterpret_cast<u64>(param.targets);
        if (count && targets > UINT64_MAX - count * sizeof(u64))
            return Code(CD::Error::PARAM_INVALID);
        for (u32 i = 0; i < count; ++i) {
            u64 address{};
            std::string target;
            if (!ReadValue(space, targets + i * sizeof(u64), address) ||
                !Target(space, address, target))
                return Code(CD::Error::PARAM_INVALID);
            message += "\n" + target;
        }
        const auto result = dialog->OpenLocalMessage(param.userId, std::move(message));
        if (!result) {
            user_data = reinterpret_cast<u64>(param.userData);
            closed_by_program = false;
            LOG_INFO(Lib_NpCommerce, "offline guest Open mode={} user={} targets={}", param.mode,
                     param.userId, count);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Code(CD::Error::OUT_OF_MEMORY);
    }
}
} // namespace Core::HostRuntime
