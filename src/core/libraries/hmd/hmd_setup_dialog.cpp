// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"

namespace Libraries::HmdSetupDialog {

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    LOG_TRACE(Lib_HmdSetupDialog, "Initialize");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    LOG_TRACE(Lib_HmdSetupDialog, "Close");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    LOG_TRACE(Lib_HmdSetupDialog, "Open");
    if (!param)
        return static_cast<s32>(Libraries::CommonDialog::Error::ARG_NULL);
    if (param->size != sizeof(*param))
        return static_cast<s32>(Libraries::CommonDialog::Error::PARAM_INVALID);
    // Android SBS explicitly advertises a virtual headset.  Desktop and
    // ordinary no-provider sessions retain the historical cancel behavior.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    LOG_TRACE(Lib_HmdSetupDialog, "GetResult");
    if (!result)
        return static_cast<s32>(Libraries::CommonDialog::Error::ARG_NULL);
    result->result = Core::HostRuntime::GuestVrSensor::Instance().Read().enabled
                         ? Libraries::CommonDialog::Result::OK
                         : Libraries::CommonDialog::Result::USER_CANCELED;
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    LOG_TRACE(Lib_HmdSetupDialog, "UpdateStatus");
    return Libraries::CommonDialog::Status::FINISHED;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    LOG_TRACE(Lib_HmdSetupDialog, "GetStatus");
    return Libraries::CommonDialog::Status::FINISHED;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    LOG_TRACE(Lib_HmdSetupDialog, "Terminate");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nmHzU4Gh0xs", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogClose);
    LIB_FUNCTION("6lVRHMV5LY0", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetResult);
    LIB_FUNCTION("J9eBpW1udl4", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetStatus);
    LIB_FUNCTION("NB1Y2kA2jCY", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogInitialize);
    LIB_FUNCTION("NNgiV4T+akU", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogOpen);
    LIB_FUNCTION("+z4OJmFreZc", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogTerminate);
    LIB_FUNCTION("Ud7j3+RDIBg", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogUpdateStatus);
};

} // namespace Libraries::HmdSetupDialog
