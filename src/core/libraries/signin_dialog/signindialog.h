// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <memory>
#include <mutex>
#include <vector>
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::SigninDialog {

enum class Status : u32 {
    NONE = 0,
    INITIALIZED = 1,
    RUNNING = 2,
    FINISHED = 3,
};

// Firmware 11.00: Open +0x1600, GetResult +0x1ae0. Unlike CommonDialog,
// this family has no BaseParam and uses its own error domain.
enum class Error : u32 {
    NotInitialized = 0x81350001,
    AlreadyInitialized = 0x81350002,
    InvalidArgument = 0x81350003,
    Internal = 0x81350004,
    InvalidState = 0x81350005,
    InvalidUser = 0x81350007,
};
struct Param {
    u32 size;
    s32 user_id;
    u32 reserved[2];
};
struct Result {
    s32 result;
};
static_assert(sizeof(Param) == 16 && sizeof(Result) == 4);

// Shared desktop/typed-host lifecycle. Only host-owned value copies enter it.
// No credentials, guest pointers or callbacks survive Open/GetResult.
class Dialog final : public std::enable_shared_from_this<Dialog> {
public:
    explicit Dialog(std::vector<s32> local_users = {}) : local_users(std::move(local_users)) {}
    ~Dialog();
    u32 Initialize();
    u32 Open(const Param* param);
    u32 Close();
    u32 Terminate();
    u32 GetResult(Result* result);
    Status GetStatus() const;
    void Cancel();
    struct Snapshot {
        Status status;
        u64 request;
        s32 user;
    };
    Snapshot Read() const;
    bool CompleteCancelled(u64 request);

private:
    void ReleaseCapture(); // mutex held
    mutable std::mutex mutex;
    std::vector<s32> local_users;
    Status status{Status::NONE};
    u64 request{};
    s32 user{};
    bool stopped{};
    bool capture{};
};

void ShowDialog(const std::shared_ptr<Dialog>& dialog);

s32 PS4_SYSV_ABI sceSigninDialogInitialize();
s32 PS4_SYSV_ABI sceSigninDialogOpen(const Param* param);
Status PS4_SYSV_ABI sceSigninDialogGetStatus();
Status PS4_SYSV_ABI sceSigninDialogUpdateStatus();
s32 PS4_SYSV_ABI sceSigninDialogGetResult(Result* result);
s32 PS4_SYSV_ABI sceSigninDialogClose();
s32 PS4_SYSV_ABI sceSigninDialogTerminate();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::SigninDialog
