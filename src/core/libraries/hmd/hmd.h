// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"
#include "core/libraries/system/userservice.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Hmd {


enum OrbisHmdDeviceStatus : u32 {
    ORBIS_HMD_DEVICE_STATUS_READY,
    ORBIS_HMD_DEVICE_STATUS_NOT_READY,
    ORBIS_HMD_DEVICE_STATUS_NOT_DETECTED,
    ORBIS_HMD_DEVICE_STATUS_NOT_READY_HMU_DISCONNECT,
};

struct OrbisHmdInitializeParam {
    void* reserved0;
    u8 reserved[8];
};

struct OrbisHmdOpenParam {
    u8 reserve[32];
};

struct OrbisHmdFieldOfView {
    float tan_out;
    float tan_in;
    float tan_top;
    float tan_bottom;
};

// Virtual headset geometry returned to the guest and used for SBS sampling.
inline constexpr OrbisHmdFieldOfView SbsFieldOfView{1.20743f, 1.181346f, 1.262872f, 1.262872f};

struct OrbisHmdPanelResolution {
    u32 width;
    u32 height;
};

struct OrbisHmdFlipToDisplayLatency {
    u16 refresh_rate_90hz;
    u16 refresh_rate_120hz;
};

struct OrbisHmdDeviceInfo {
    OrbisHmdPanelResolution panel_resolution;
    OrbisHmdFlipToDisplayLatency flip_to_display_latency;
};

struct OrbisHmdDeviceInformation {
    OrbisHmdDeviceStatus status;
    Libraries::UserService::OrbisUserServiceUserId user_id;
    u8 reserve0[4];
    OrbisHmdDeviceInfo device_info;
    u8 hmu_mount;
    u8 reserve1[7];
};

struct OrbisHmdEyeOffset {
    float offset_x;
    float offset_y;
    float offset_z;
    u8 reserve[20];
};

static_assert(sizeof(OrbisHmdFieldOfView) == 0x10);
static_assert(sizeof(OrbisHmdDeviceInfo) == 0x0c);
static_assert(sizeof(OrbisHmdDeviceInformation) == 0x20);
static_assert(sizeof(OrbisHmdEyeOffset) == 0x20);

// Firmware 11.00 validates this object in sceHmdReprojectionInitialize.
// The first two qwords are consumed as handles/buffer roots by the firmware;
// their pointee types are not recoverable from the public export.  Firmware
// validates the dwords at 0x20 and 0x24 as selectors and requires 0x28, 0x2c
// and 0x30 to be zero.  Beat Saber prepares exactly 0x38 bytes before the
// call.  Keep the unknown fields explicit instead of passing a guessed native
// pointer or a zero-argument scalar veneer.
struct OrbisHmdReprojectionInitializeParam {
    u64 opaque0;
    u64 opaque8;
    u64 field10;
    u64 field18;
    u32 selector20;
    u32 selector24;
    u32 reserved28;
    u32 reserved2c;
    u32 reserved30;
    u32 reserved34;
};

static_assert(sizeof(OrbisHmdInitializeParam) == 0x10);
static_assert(sizeof(OrbisHmdOpenParam) == 0x20);
static_assert(sizeof(OrbisHmdReprojectionInitializeParam) == 0x38);
static_assert(offsetof(OrbisHmdReprojectionInitializeParam, selector20) == 0x20);
static_assert(offsetof(OrbisHmdReprojectionInitializeParam, reserved30) == 0x30);

// Partial wire layouts from firmware 11.00 +0x17950/+0x17bd0. All address
// fields remain numeric guest addresses, never dereferenceable host pointers.
// Copying these records alone does NOT validate their nested GPU resources.
struct OrbisHmdReprojectionLayer {
    u64 roots00[4];
    u64 root20;
    // Firmware 11.00 +0x1824f/+0x1825c copies these two float4s to
    // StartParam +0x18/+0x28 in the single colour-layer path (kind 0).
    // They map ray tangents to source UVs; they are not destination viewports.
    float color_tan_to_uv[2][4];
    u8 opaque48[0x20]; // not consumed by that firmware path; other kinds unproved
    float field68;
    float field6c;
    u64 optional70;
    u32 kind78;
    u32 reserved7c[9];
    u64 reserved_a0;
};

struct OrbisHmdReprojectionSubmission {
    u64 root00;
    u32 selector08;
    u32 opaque0c;
    u64 opaque10;
    u32 selector18;
    u32 opaque1c;
    u64 flags20;
    u64 reserved28[5];
};

// Firmware 11.00: +0x161e0, +0x16c90 and +0x17690. Numeric addresses
// reference 32-byte texture descriptors and a 16-byte GNM sampler.
struct OrbisHmdReprojectionStartParam {
    u64 textures[2], sampler;
    float tan_to_uv[2][4]; // scale X/Y, bias X/Y for each eye
    u64 release_label;
    u32 selector, padding44;
    u64 opaque48;
    u32 selector50, padding54;
    u64 flags;
    u64 reserved[4];
};
struct OrbisHmdReprojectionOverlayParam {
    u64 textures[2], sampler;
    float tan_to_uv[2][4];
    u32 reserved[9];
    u32 padding5c;
};
struct OrbisHmdReprojection2dParam {
    u64 texture, sampler;
    float uv[4]; // normalized scale X/Y, bias X/Y
    u64 release_label;
    u32 selector, padding2c;
    u64 reserved[4];
};
static_assert(sizeof(OrbisHmdReprojectionStartParam) == 0x80);
static_assert(offsetof(OrbisHmdReprojectionStartParam, release_label) == 0x38);
static_assert(offsetof(OrbisHmdReprojectionStartParam, flags) == 0x58);
static_assert(sizeof(OrbisHmdReprojectionOverlayParam) == 0x60);
static_assert(sizeof(OrbisHmdReprojection2dParam) == 0x50);

static_assert(sizeof(OrbisHmdReprojectionLayer) == 0xa8);
static_assert(offsetof(OrbisHmdReprojectionLayer, color_tan_to_uv) == 0x28);
static_assert(offsetof(OrbisHmdReprojectionLayer, opaque48) == 0x48);
static_assert(offsetof(OrbisHmdReprojectionLayer, root20) == 0x20);
static_assert(offsetof(OrbisHmdReprojectionLayer, optional70) == 0x70);
static_assert(offsetof(OrbisHmdReprojectionLayer, kind78) == 0x78);
static_assert(offsetof(OrbisHmdReprojectionLayer, reserved7c) == 0x7c);
static_assert(offsetof(OrbisHmdReprojectionLayer, reserved_a0) == 0xa0);
static_assert(sizeof(OrbisHmdReprojectionSubmission) == 0x50);
static_assert(offsetof(OrbisHmdReprojectionSubmission, selector18) == 0x18);
static_assert(offsetof(OrbisHmdReprojectionSubmission, flags20) == 0x20);

// Reprojection.  These are deliberately raw guest-facing records until the
// layer/submit layouts are fully recovered; the HLE must not reinterpret them
// as host pointers.
s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer(const void* layers, u32 layer_count,
                                               const void* submission,
                                               const void* shared_layer_data,
                                               u64 opaque_arg4, const void* reserved);
s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer();
s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd();
s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart();
s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo();
s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer();
s32 PS4_SYSV_ABI sceHmdReprojectionFinalize();
s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(
    const OrbisHmdReprojectionInitializeParam* param, u32 mode, const void* reserved);
s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize();
s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback();
s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(s32 video_handle, s32 buffer_index0,
                                                     s32 buffer_index1, const void* reserved);
s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor(float red, float green, float blue);
s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd();
s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart();
s32 PS4_SYSV_ABI sceHmdReprojectionStart(const OrbisHmdReprojectionStartParam* param,
                                       const void* pose, u64 sequence, const void* reserved);
s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr(const OrbisHmdReprojection2dParam* param,
                                           u64 sequence, const void* reserved);
s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2();
s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear();
s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay();
s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay(const OrbisHmdReprojectionStartParam* param,
    const void* pose, u64 sequence, const OrbisHmdReprojectionOverlayParam* overlay,
    const void* reserved);
s32 PS4_SYSV_ABI sceHmdReprojectionStop();
s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback();
s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers();

// Distortion
s32 PS4_SYSV_ABI sceHmdDistortionGet2dVrCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetCompoundEyeCorrectionCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetCorrectionCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetWideNearCorrectionCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetWorkMemoryAlign();
s32 PS4_SYSV_ABI sceHmdDistortionGetWorkMemorySize();
s32 PS4_SYSV_ABI sceHmdDistortionInitialize(void* reserved);
s32 PS4_SYSV_ABI sceHmdDistortionSetOutputMinColor();
s32 PS4_SYSV_ABI sceHmdDistortionTerminate();
s32 PS4_SYSV_ABI sceHmdGetDistortionParams();
s32 PS4_SYSV_ABI Func_B26430EA74FC3DC0();
s32 PS4_SYSV_ABI Func_B614F290B67FB59B();

// libSceHmd
s32 PS4_SYSV_ABI sceHmdClose(s32 handle);
s32 PS4_SYSV_ABI sceHmdGet2DEyeOffset(s32 handle, OrbisHmdEyeOffset* left_offset,
                                      OrbisHmdEyeOffset* right_offset);
s32 PS4_SYSV_ABI sceHmdGetAssyError(void* data);
s32 PS4_SYSV_ABI sceHmdGetDeviceInformation(OrbisHmdDeviceInformation* info);
s32 PS4_SYSV_ABI sceHmdGetDeviceInformationByHandle(s32 handle, OrbisHmdDeviceInformation* info);

s32 PS4_SYSV_ABI sceHmdGetFieldOfView(s32 handle, OrbisHmdFieldOfView* field_of_view);
s32 PS4_SYSV_ABI sceHmdGetInertialSensorData(s32 handle, void* data, s32 unk);
s32 PS4_SYSV_ABI sceHmdInitialize(const OrbisHmdInitializeParam* param);
s32 PS4_SYSV_ABI sceHmdInitialize315(const OrbisHmdInitializeParam* param);
s32 PS4_SYSV_ABI sceHmdOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                            s32 index, OrbisHmdOpenParam* param);
s32 PS4_SYSV_ABI sceHmdTerminate();

// Internal
s32 PS4_SYSV_ABI sceHmdInternal3dAudioClose();
s32 PS4_SYSV_ABI sceHmdInternal3dAudioOpen();
s32 PS4_SYSV_ABI sceHmdInternal3dAudioSendData();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenClose();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenGetAudioStatus();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenGetFadeState();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenGetVideoStatus();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenOpen();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenSendAudio();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenSendVideo();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenSetFadeAndSwitch();
s32 PS4_SYSV_ABI sceHmdInternalBindDeviceWithUserId();
s32 PS4_SYSV_ABI sceHmdInternalCheckDeviceModelMk3();
s32 PS4_SYSV_ABI sceHmdInternalCheckS3dPassModeAvailable();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportCancel();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportClose();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportOpen();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportReadData();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportReadDataSize();
s32 PS4_SYSV_ABI sceHmdInternalCreateSharedMemory();
s32 PS4_SYSV_ABI sceHmdInternalDfuCheckAfterPvt();
s32 PS4_SYSV_ABI sceHmdInternalDfuCheckPartialUpdateAvailable();
s32 PS4_SYSV_ABI sceHmdInternalDfuClose();
s32 PS4_SYSV_ABI sceHmdInternalDfuGetStatus();
s32 PS4_SYSV_ABI sceHmdInternalDfuOpen();
s32 PS4_SYSV_ABI sceHmdInternalDfuReset();
s32 PS4_SYSV_ABI sceHmdInternalDfuSend();
s32 PS4_SYSV_ABI sceHmdInternalDfuSendSize();
s32 PS4_SYSV_ABI sceHmdInternalDfuSetMode();
s32 PS4_SYSV_ABI sceHmdInternalDfuStart();
s32 PS4_SYSV_ABI sceHmdInternalEventInitialize();
s32 PS4_SYSV_ABI sceHmdInternalGetBrightness();
s32 PS4_SYSV_ABI sceHmdInternalGetCrashDumpInfo();
s32 PS4_SYSV_ABI sceHmdInternalGetDebugMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDebugSocialScreenMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDebugTextMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDefaultLedData();
s32 PS4_SYSV_ABI sceHmdInternalGetDemoMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDeviceInformation();
s32 PS4_SYSV_ABI sceHmdInternalGetDeviceInformationByHandle();
s32 PS4_SYSV_ABI sceHmdInternalGetDeviceStatus(OrbisHmdDeviceStatus* status);
s32 PS4_SYSV_ABI sceHmdInternalGetEyeStatus();
s32 PS4_SYSV_ABI sceHmdInternalGetHmuOpticalParam();
s32 PS4_SYSV_ABI sceHmdInternalGetHmuPowerStatusForDebug();
s32 PS4_SYSV_ABI sceHmdInternalGetHmuSerialNumber();
s32 PS4_SYSV_ABI sceHmdInternalGetIPD();
s32 PS4_SYSV_ABI sceHmdInternalGetIpdSettingEnableForSystemService();
s32 PS4_SYSV_ABI sceHmdInternalGetPuBuildNumber();
s32 PS4_SYSV_ABI sceHmdInternalGetPuPositionParam();
s32 PS4_SYSV_ABI sceHmdInternalGetPuRevision();
s32 PS4_SYSV_ABI sceHmdInternalGetPUSerialNumber();
s32 PS4_SYSV_ABI sceHmdInternalGetPUVersion();
s32 PS4_SYSV_ABI sceHmdInternalGetRequiredPUPVersion();
s32 PS4_SYSV_ABI sceHmdInternalGetStatusReport();
s32 PS4_SYSV_ABI sceHmdInternalGetTv4kCapability();
s32 PS4_SYSV_ABI sceHmdInternalGetVirtualDisplayDepth();
s32 PS4_SYSV_ABI sceHmdInternalGetVirtualDisplayHeight();
s32 PS4_SYSV_ABI sceHmdInternalGetVirtualDisplaySize();
s32 PS4_SYSV_ABI sceHmdInternalGetVr2dData();
s32 PS4_SYSV_ABI sceHmdInternalIsCommonDlgMiniAppVr2d();
s32 PS4_SYSV_ABI sceHmdInternalIsCommonDlgVr2d();
s32 PS4_SYSV_ABI sceHmdInternalIsGameVr2d();
s32 PS4_SYSV_ABI sceHmdInternalIsMiniAppVr2d();
s32 PS4_SYSV_ABI sceHmdInternalMapSharedMemory();
s32 PS4_SYSV_ABI sceHmdInternalMirroringModeSetAspect();
s32 PS4_SYSV_ABI sceHmdInternalMirroringModeSetAspectDebug();
s32 PS4_SYSV_ABI sceHmdInternalMmapGetCount();
s32 PS4_SYSV_ABI sceHmdInternalMmapGetModeId();
s32 PS4_SYSV_ABI sceHmdInternalMmapGetSensorCalibrationData();
s32 PS4_SYSV_ABI sceHmdInternalMmapIsConnect();
s32 PS4_SYSV_ABI sceHmdInternalPushVr2dData();
s32 PS4_SYSV_ABI sceHmdInternalRegisterEventCallback();
s32 PS4_SYSV_ABI sceHmdInternalResetInertialSensor();
s32 PS4_SYSV_ABI sceHmdInternalResetLedForVrTracker();
s32 PS4_SYSV_ABI sceHmdInternalResetLedForVsh();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeClose();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeGetAudioStatus();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeGetVideoStatus();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeOpen();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeSendAudio();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeSendVideo();
s32 PS4_SYSV_ABI sceHmdInternalSetBrightness();
s32 PS4_SYSV_ABI sceHmdInternalSetCrashReportCommand();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugGpo();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugSocialScreenMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugTextMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDefaultLedData();
s32 PS4_SYSV_ABI sceHmdInternalSetDemoMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDeviceConnection();
s32 PS4_SYSV_ABI sceHmdInternalSetForcedCrash();
s32 PS4_SYSV_ABI sceHmdInternalSetHmuPowerControl();
s32 PS4_SYSV_ABI sceHmdInternalSetHmuPowerControlForDebug();
s32 PS4_SYSV_ABI sceHmdInternalSetIPD();
s32 PS4_SYSV_ABI sceHmdInternalSetIpdSettingEnableForSystemService();
s32 PS4_SYSV_ABI sceHmdInternalSetLedOn();
s32 PS4_SYSV_ABI sceHmdInternalSetM2LedBrightness();
s32 PS4_SYSV_ABI sceHmdInternalSetM2LedOn();
s32 PS4_SYSV_ABI sceHmdInternalSetPortConnection();
s32 PS4_SYSV_ABI sceHmdInternalSetPortStatus();
s32 PS4_SYSV_ABI sceHmdInternalSetS3dPassMode();
s32 PS4_SYSV_ABI sceHmdInternalSetSidetone();
s32 PS4_SYSV_ABI sceHmdInternalSetUserType();
s32 PS4_SYSV_ABI sceHmdInternalSetVirtualDisplayDepth();
s32 PS4_SYSV_ABI sceHmdInternalSetVirtualDisplayHeight();
s32 PS4_SYSV_ABI sceHmdInternalSetVirtualDisplaySize();
s32 PS4_SYSV_ABI sceHmdInternalSetVRMode();
s32 PS4_SYSV_ABI sceHmdInternalSocialScreenGetFadeState();
s32 PS4_SYSV_ABI sceHmdInternalSocialScreenSetFadeAndSwitch();
s32 PS4_SYSV_ABI sceHmdInternalSocialScreenSetOutput();

s32 PS4_SYSV_ABI Func_202D0D1A687FCD2F();
s32 PS4_SYSV_ABI Func_358DBF818A3D8A12();
s32 PS4_SYSV_ABI Func_5CCBADA76FE8F40E();
s32 PS4_SYSV_ABI Func_63D403167DC08CF0();
s32 PS4_SYSV_ABI Func_69383B2B4E3AEABF(s32 handle, void* data, s32 unk);
s32 PS4_SYSV_ABI Func_791560C32F4F6D68();
s32 PS4_SYSV_ABI Func_7C955961EA85B6D3();
s32 PS4_SYSV_ABI Func_9952277839236BA7();
s32 PS4_SYSV_ABI Func_9A276E739E54EEAF();
s32 PS4_SYSV_ABI Func_9E501994E289CBE7();
s32 PS4_SYSV_ABI Func_A31A0320D80EAD99();
s32 PS4_SYSV_ABI Func_A31F4DA8B3BD2E12();
s32 PS4_SYSV_ABI Func_A92D7C23AC364993();
s32 PS4_SYSV_ABI Func_ADCCC25CB876FDBE();
s32 PS4_SYSV_ABI Func_B16652641FE69F0E();
s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49();
s32 PS4_SYSV_ABI Func_FC193BD653F2AF2E();
s32 PS4_SYSV_ABI Func_FF2E0E53015FE231();

void RegisterDistortion(Core::Loader::SymbolsResolver* sym);
void RegisterReprojection(Core::Loader::SymbolsResolver* sym);
void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Hmd
