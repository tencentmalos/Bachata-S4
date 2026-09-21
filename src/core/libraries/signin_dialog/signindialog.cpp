// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/user_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "signindialog.h"

namespace Libraries::SigninDialog {
namespace {
u32 Code(Error error) {
    return static_cast<u32>(error);
}
std::shared_ptr<Dialog> DesktopDialog() {
    static auto dialog = [] {
        std::vector<s32> users;
        for (const auto& user : UserManagement.GetAllUsers())
            users.push_back(user.user_id);
        return std::make_shared<Dialog>(std::move(users));
    }();
    return dialog;
}
} // namespace
Dialog::~Dialog() {
    Cancel();
}
void Dialog::ReleaseCapture() {
    if (capture) {
        ImGui::Core::ReleaseGamepadInputCapture();
        capture = false;
    }
}
u32 Dialog::Initialize() {
    std::lock_guard lock(mutex);
    if (stopped)
        return Code(Error::InvalidState);
    if (status != Status::NONE)
        return Code(Error::AlreadyInitialized);
    status = Status::INITIALIZED;
    LOG_INFO(Lib_SigninDialog, "Initialize: offline provider ready");
    return 0;
}
u32 Dialog::Open(const Param* param) {
    if (!param || param->size != sizeof(Param) || param->reserved[0] || param->reserved[1])
        return Code(Error::InvalidArgument);
    // Firmware accepts [0x10000000, 0x3fffffff]. Also accept the actual emulator
    // local profile IDs (e.g. 1000), just as the UserService/IME adapters do.
    const auto id = static_cast<u32>(param->user_id);
    if ((id < 0x10000000 || id > 0x3fffffff) &&
        std::ranges::find(local_users, param->user_id) == local_users.end())
        return Code(Error::InvalidUser);
    const auto self = weak_from_this().lock();
    if (!self)
        return Code(Error::Internal);
    {
        std::lock_guard lock(mutex);
        if (stopped)
            return Code(Error::InvalidState);
        if (status == Status::NONE)
            return Code(Error::NotInitialized);
        if (status == Status::RUNNING)
            return Code(Error::InvalidState);
        status = Status::RUNNING;
        user = param->user_id;
        ++request;
        ImGui::Core::AcquireGamepadInputCapture();
        capture = true;
        LOG_INFO(Lib_SigninDialog, "Open: request={} user={} offline prompt", request, user);
    }
    ShowDialog(self);
    return 0;
}
u32 Dialog::Close() {
    std::lock_guard lock(mutex);
    if (status == Status::NONE)
        return Code(Error::NotInitialized);
    if (status == Status::RUNNING)
        status = Status::FINISHED;
    ReleaseCapture();
    return 0;
}
u32 Dialog::Terminate() {
    std::lock_guard lock(mutex);
    if (status == Status::NONE)
        return Code(Error::NotInitialized);
    status = Status::NONE;
    ReleaseCapture();
    LOG_INFO(Lib_SigninDialog, "Terminate");
    return 0;
}
u32 Dialog::GetResult(Result* output) {
    if (!output)
        return Code(Error::InvalidArgument);
    std::lock_guard lock(mutex);
    if (status == Status::NONE)
        return Code(Error::NotInitialized);
    // Firmware writes cancel before its finished-state check. Never change NP
    // account state or fabricate a successful PSN identity.
    output->result = 1;
    if (status != Status::FINISHED)
        return Code(Error::InvalidState);
    LOG_INFO(Lib_SigninDialog, "GetResult: request={} result=1 (cancelled/offline)", request);
    return 0;
}
Status Dialog::GetStatus() const {
    std::lock_guard lock(mutex);
    return status;
}
Dialog::Snapshot Dialog::Read() const {
    std::lock_guard lock(mutex);
    return {status, request, user};
}
bool Dialog::CompleteCancelled(u64 expected) {
    std::lock_guard lock(mutex);
    if (stopped || status != Status::RUNNING || expected != request)
        return false;
    status = Status::FINISHED;
    ReleaseCapture();
    LOG_INFO(Lib_SigninDialog, "Dismiss: request={} user={} result=1", request, user);
    return true;
}
void Dialog::Cancel() {
    std::lock_guard lock(mutex);
    stopped = true;
    status = Status::NONE;
    ReleaseCapture();
}
s32 PS4_SYSV_ABI sceSigninDialogInitialize() {
    return DesktopDialog()->Initialize();
}
s32 PS4_SYSV_ABI sceSigninDialogOpen(const Param* param) {
    return DesktopDialog()->Open(param);
}
Status PS4_SYSV_ABI sceSigninDialogGetStatus() {
    return DesktopDialog()->GetStatus();
}
Status PS4_SYSV_ABI sceSigninDialogUpdateStatus() {
    return DesktopDialog()->GetStatus();
}
s32 PS4_SYSV_ABI sceSigninDialogGetResult(Result* result) {
    return DesktopDialog()->GetResult(result);
}
s32 PS4_SYSV_ABI sceSigninDialogClose() {
    return DesktopDialog()->Close();
}
s32 PS4_SYSV_ABI sceSigninDialogTerminate() {
    return DesktopDialog()->Terminate();
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("mlYGfmqE3fQ", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogInitialize);
    LIB_FUNCTION("JlpJVoRWv7U", "libSceSigninDialog", 1, "libSceSigninDialog", sceSigninDialogOpen);
    LIB_FUNCTION("2m077aeC+PA", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogGetStatus);
    LIB_FUNCTION("Bw31liTFT3A", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogUpdateStatus);
    LIB_FUNCTION("nqG7rqnYw1U", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogGetResult);
    LIB_FUNCTION("M3OkENHcyiU", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogClose);
    LIB_FUNCTION("LXlmS6PvJdU", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogTerminate);
};

} // namespace Libraries::SigninDialog
