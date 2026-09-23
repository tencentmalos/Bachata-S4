// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "common/logging/log.h"
#include "core/libraries/system/msgdialog_ui.h"
#include "guest_msg_dialog.h"
#include "imgui/renderer/imgui_core.h"

namespace Core::HostRuntime {
namespace CD = Libraries::CommonDialog;
namespace MD = Libraries::MsgDialog;
namespace {
template <class T>
bool ReadValue(GuestCpu::GuestAddressSpace& space, u64 address, T& value) {
    return address && bool(space.ReadData({address}, std::as_writable_bytes(std::span{&value, 1})));
}
template <class T>
u64 Address(T* pointer) {
    return reinterpret_cast<u64>(pointer);
}
bool ReadString(GuestCpu::GuestAddressSpace& space, u64 address, std::string& out,
                size_t limit = 8191) {
    if (!address || address > UINT64_MAX - limit)
        return false;
    for (size_t i = 0; i <= limit; ++i) {
        char ch{};
        if (!ReadValue(space, address + i, ch))
            return false;
        if (!ch)
            return true;
        if (i == limit)
            return false;
        out += ch;
    }
    return false;
}
template <class T>
bool Zero(const T& bytes) {
    return std::ranges::all_of(bytes, [](auto v) { return v == 0; });
}
constexpr u32 Code(CD::Error error) {
    return static_cast<u32>(error);
}
static_assert(sizeof(MD::OrbisParam) == 136 && sizeof(MD::DialogResult) == 44);
static_assert(sizeof(MD::UserMessageParam) == 48 && sizeof(MD::ButtonsParam) == 48);
static_assert(sizeof(MD::ProgressBarParam) == 80 && sizeof(MD::SystemMessageParam) == 36);
} // namespace

GuestMsgDialog::~GuestMsgDialog() {
    Cancel();
}
bool GuestMsgDialog::IsNid(std::string_view nid) {
    return std::ranges::find(Nids, nid) != std::end(Nids);
}
void GuestMsgDialog::ReleaseCapture() {
    if (capture) {
        ImGui::Core::ReleaseGamepadInputCapture();
        capture = false;
    }
}
void GuestMsgDialog::Cancel() {
    std::lock_guard lock(mutex);
    stopped = true;
    state.status = Status::NONE;
    common->Release(this);
    ReleaseCapture();
}
GuestMsgDialog::Snapshot GuestMsgDialog::Read() const {
    std::lock_guard lock(mutex);
    return state;
}
u32 GuestMsgDialog::Publish(Snapshot next) {
    auto self = weak_from_this().lock();
    if (!self)
        return Code(CD::Error::UNEXPECTED_FATAL);
    next.status = Status::RUNNING;
    next.request = state.request + 1;
    state = std::move(next);
    result = button = 0;
    if (silent && state.acknowledgement) {
        // Complete the same acknowledgement as Respond(request, 1), without
        // creating a modal or taking input capture. Keep the CommonDialog lease
        // until Terminate, just as in the interactive lifecycle.
        result = 0;
        button = 1;
        state.status = Status::FINISHED;
        try {
            const auto record = nlohmann::json{{"request", state.request}, {"user", state.user},
                {"title", state.title}, {"text", state.text}, {"mode", state.mode},
                {"action", "acknowledge"}, {"result", result}, {"button", button}}.dump();
            LOG_INFO(Lib_MsgDlg, "DIALOG_AUTO_ACK {}", record);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "GuestDialog", "DIALOG_AUTO_ACK %s", record.c_str());
#endif
        } catch (...) {} // Logging cannot turn an acknowledgement into an Open failure.
        return 0;
    }
    capture = true;
    ImGui::Core::AcquireGamepadInputCapture();
    LOG_INFO(Lib_MsgDlg, "guest Open request={} mode={} user={}", state.request, state.mode,
             state.user);
    ShowGuestMsgDialog(self);
    return 0;
}
u32 GuestMsgDialog::OpenLocalMessage(s32 user, std::string text, std::string title) {
    try {
        std::lock_guard lock(mutex);
        if (stopped || (state.status != Status::INITIALIZED && state.status != Status::FINISHED))
            return Code(CD::Error::INVALID_STATE);
        if (std::ranges::find(users, user) == users.end())
            return Code(CD::Error::PARAM_INVALID);
        Snapshot next;
        next.user = user;
        next.mode = 1;
        next.text = std::move(text);
        next.title = std::move(title);
        next.first = "Close";
        next.acknowledgement = true;
        next.cancel = true;
        return Publish(std::move(next));
    } catch (const std::bad_alloc&) {
        return Code(CD::Error::OUT_OF_MEMORY);
    }
}
bool GuestMsgDialog::Respond(u64 request, u32 action) {
    std::lock_guard lock(mutex);
    if (stopped || state.status != Status::RUNNING || state.request != request || action > 2)
        return false;
    if ((!action && !state.cancel) || (action == 1 && state.first.empty()) ||
        (action == 2 && state.second.empty()))
        return false;
    const bool cancelled = !action || (action == 2 && state.cancel);
    result = cancelled ? 1 : 0;
    button = cancelled ? 0 : action;
    state.status = Status::FINISHED;
    ReleaseCapture();
    LOG_INFO(Lib_MsgDlg, "guest response request={} result={} button={}", request, result, button);
    return true;
}
u64 GuestMsgDialog::Invoke(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                           const std::array<u64, 6>& args) {
    try {
        std::unique_lock lock(mutex);
        if (stopped)
            return Code(CD::Error::INVALID_STATE);
        if (nid == "CWVW78Qc3fI" || nid == "6fIC3XKt2k0")
            return u32(state.status);
        if (nid == "lDqxaY1UbEo") {
            const auto acquired = common->Acquire(this);
            if (acquired != CD::Error::OK)
                return Code(acquired);
            state.status = Status::INITIALIZED;
            return 0;
        }
        if (nid == "ePw-kqZmelo") {
            if (state.status == Status::NONE)
                return Code(CD::Error::NOT_INITIALIZED);
            state.status = Status::NONE;
            common->Release(this);
            ReleaseCapture();
            return 0;
        }
        if (nid == "HTrcDKlFKuM") {
            if (state.status != Status::RUNNING)
                return Code(CD::Error::NOT_RUNNING);
            state.status = Status::FINISHED;
            result = 0;
            button = 0;
            ReleaseCapture();
            return 0;
        }
        if (nid == "Lr8ovHH9l6A") {
            if (state.status != Status::FINISHED)
                return Code(CD::Error::NOT_FINISHED);
            if (!args[0])
                return Code(CD::Error::ARG_NULL);
            auto output = space.AcquireDataSpan({{args[0]}, sizeof(MD::DialogResult)}, true);
            if (!output)
                return Code(CD::Error::ARG_NULL);
            MD::DialogResult value{};
            value.result = static_cast<CD::Result>(result);
            value.buttonId = static_cast<MD::ButtonId>(button);
            std::memcpy(output.Value().WritableBytes().data(), &value, sizeof(value));
            return 0;
        }
        if (nid == "Gc5k1qcK4fs" || nid == "wTpfglkmv34" || nid == "6H-71OdrpXM") {
            if (state.status != Status::RUNNING)
                return Code(CD::Error::NOT_RUNNING);
            if (state.mode != 2)
                return Code(CD::Error::NOT_SUPPORTED);
            if (u32(args[0]) != 0)
                return Code(CD::Error::PARAM_INVALID);
            if (nid == "6H-71OdrpXM") {
                std::string text;
                if (!ReadString(space, args[1], text))
                    return Code(CD::Error::PARAM_INVALID);
                state.text = std::move(text);
            } else {
                // Firmware 11.00 +0xece uses u32 addition then caps Inc at 100;
                // SetValue (+0xfa3) stores the raw u32. Clamp only UI rendering.
                state.progress = nid == "Gc5k1qcK4fs"
                                     ? std::min<u32>(state.progress + u32(args[1]), 100)
                                     : u32(args[1]);
            }
            return 0;
        }
        if (nid != "b06Hh0DPEaE")
            return Code(CD::Error::NOT_SUPPORTED);
        if (state.status != Status::INITIALIZED && state.status != Status::FINISHED)
            return Code(CD::Error::INVALID_STATE);
        MD::OrbisParam param{};
        if (!ReadValue(space, args[0], param))
            return Code(CD::Error::ARG_NULL);
        if (param.size != sizeof(param) || param.baseParam.size != sizeof(param.baseParam) ||
            param.baseParam.magic != u32(args[0] + 0xc0d1a109) || !Zero(param.baseParam.reserved) ||
            !Zero(param.reserved))
            return Code(CD::Error::PARAM_INVALID);
        Snapshot next;
        next.user = param.userId;
        if (next.user == -1 && !users.empty())
            next.user = users.front();
        if (std::ranges::find(users, next.user) == users.end())
            return Code(CD::Error::PARAM_INVALID);
        next.mode = u32(param.mode);
        if (param.mode == MD::MsgDialogMode::USER_MSG) {
            if (param.progBarParam || param.sysMsgParam)
                return Code(CD::Error::PARAM_INVALID);
            MD::UserMessageParam user{};
            if (!ReadValue(space, Address(param.userMsgParam), user) || !Zero(user.reserved) ||
                !ReadString(space, Address(user.msg), next.text))
                return Code(CD::Error::PARAM_INVALID);
            next.button_type = u32(user.buttonType);
            switch (user.buttonType) {
            case MD::ButtonType::OK:
                next.acknowledgement = true;
                next.first = "OK";
                break;
            case MD::ButtonType::YESNO:
            case MD::ButtonType::YESNO_FOCUS_NO:
                next.first = "Yes";
                next.second = "No";
                break;
            case MD::ButtonType::OK_CANCEL:
            case MD::ButtonType::OK_CANCEL_FOCUS_CANCEL:
                next.first = "OK";
                next.second = "Cancel";
                next.cancel = true;
                break;
            case MD::ButtonType::NONE:
            case MD::ButtonType::WAIT:
                break; // wait is not a successful action
            case MD::ButtonType::WAIT_CANCEL:
                next.cancel = true;
                break;
            case MD::ButtonType::TWO_BUTTONS: {
                MD::ButtonsParam buttons{};
                if (!ReadValue(space, Address(user.buttonsParam), buttons) ||
                    !Zero(buttons.reserved) ||
                    !ReadString(space, Address(buttons.msg1), next.first, 63) ||
                    !ReadString(space, Address(buttons.msg2), next.second, 63) ||
                    next.first.empty() || next.second.empty())
                    return Code(CD::Error::PARAM_INVALID);
                break;
            }
            default:
                return Code(CD::Error::PARAM_INVALID);
            }
            next.focus_second = user.buttonType == MD::ButtonType::YESNO_FOCUS_NO ||
                                user.buttonType == MD::ButtonType::OK_CANCEL_FOCUS_CANCEL;
        } else if (param.mode == MD::MsgDialogMode::PROGRESS_BAR) {
            if (param.userMsgParam || param.sysMsgParam)
                return Code(CD::Error::PARAM_INVALID);
            MD::ProgressBarParam progress{};
            if (!ReadValue(space, Address(param.progBarParam), progress) ||
                !Zero(progress.reserved) || u32(progress.barType) > 1 ||
                !ReadString(space, Address(progress.msg), next.text))
                return Code(CD::Error::PARAM_INVALID);
            next.cancel = progress.barType == MD::ProgressBarType::PERCENTAGE_CANCEL;
        } else if (param.mode == MD::MsgDialogMode::SYSTEM_MSG) {
            if (param.userMsgParam || param.progBarParam)
                return Code(CD::Error::PARAM_INVALID);
            MD::SystemMessageParam system{};
            if (!ReadValue(space, Address(param.sysMsgParam), system) || !Zero(system.reserved))
                return Code(CD::Error::PARAM_INVALID);
            switch (system.sysMsgType) {
            case MD::SystemMessageType::TRC_EMPTY_STORE:
                next.text = "No content is available in the store.";
                break;
            case MD::SystemMessageType::TRC_PSN_CHAT_RESTRICTION:
                next.text = "Chat is restricted for this user.";
                break;
            case MD::SystemMessageType::TRC_PSN_UGC_RESTRICTION:
                next.text = "User-generated content is restricted for this user.";
                break;
            case MD::SystemMessageType::CAMERA_NOT_CONNECTED:
                next.text = "Camera not connected.";
                break;
            case MD::SystemMessageType::WARNING_PROFILE_PICTURE_AND_NAME_NOT_SHARED:
                next.text = "Profile picture and name are not shared.";
                break;
            default:
                return Code(CD::Error::NOT_SUPPORTED);
            }
            next.first = "OK";
            next.acknowledgement = true;
        } else
            return Code(CD::Error::PARAM_INVALID);
        return Publish(std::move(next));
    } catch (const std::bad_alloc&) {
        return Code(CD::Error::OUT_OF_MEMORY);
    }
}
} // namespace Core::HostRuntime
