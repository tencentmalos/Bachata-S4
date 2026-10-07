// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/camera/camera.h"
#include "core/libraries/camera/camera_error.h"

namespace Core::HostRuntime {
inline constexpr std::string_view CameraNids[]{
    "OMS9LlcrvBo", "ztqH5qNTpTk", "p6n3Npi3YY4", "VQ+5kAqsE2Q", "9+SNhbctk64",
    "BHn83xrF92E", "9EpRYMy7rHU", "cLxF1QtHch0", "U3BVwQl2R5Q", "wpeyFwJ+UEI",
    "8WtmqmE4edw", "mxgMmR+1Kr0", "2G2C0nmd++M", "+X1Kgnn3bzg",
    // SetExposureGain, GetExposureGain, GetCalibrationData, SetAutoWhiteBalance
    "wgBMXJJA6K4", "ObIste7hqdk", "RHYJ7GKOSMg", "zIKL4kZleuc"};
inline bool IsCameraNid(std::string_view nid) {
    return std::find(std::begin(CameraNids), std::end(CameraNids), nid) != std::end(CameraNids);
}

// SBS diagnostic Camera: desktop ABI/configuration table, fixed black pixels.
// No SDL/host pointer reaches the guest. Pool mappings belong to the runtime and
// remain resident until session teardown, including after Close (GPU readers).
//
// Frame pacing (AstroQuest v0.18 9ff3e43, core/libraries/camera/camera.cpp:57-79): a read that
// asks for the next frame blocks until the sensor delivers one, at most every 1/60 s, and the
// sensor never runs ahead of the title: a new frame waits for the title to present once more
// (at most 200 ms, so a title that presents nothing still gets frames). Titles read the camera
// into a ring of three entries on one thread while the main thread uses the newest complete one;
// a sensor running ahead of a slow title would wipe the entry being used. The wait holds no lock
// and no pin and ends on Stop. The exposure and gain a title sets are reported back in each
// frame's metadata, which titles read to tell how a frame was captured.
class GuestCamera final {
    using Frame = Libraries::Camera::OrbisCameraFrameData;
    using Config = Libraries::Camera::OrbisCameraConfigExtention;
    using ExposureGain = Libraries::Camera::OrbisCameraExposureGain;
    static constexpr u64 MaxPlaneBytes = 1280 * 800 * 2;
    static constexpr u64 FramePeriodUs = 1000000 / 60;
    static constexpr auto GuestFrameTimeout = std::chrono::milliseconds{200};
    // Desktop sceCameraGetExposureGain's defaults.
    static constexpr ExposureGain DefaultExposureGain{0, 20, 100, 0};
    GuestCpu::GuestAddressSpace& space;
    std::function<u64(u64)> allocate;
    const u32 sdk;
    // The title's presented frame count, and process time in microseconds.
    std::function<u64()> guest_frames;
    std::function<u64()> now_us;
    std::mutex mutex;
    std::condition_variable_any wake;
    u64 pool{}, sequence{};
    s32 handle{}, next_handle{1};
    bool started{};
    std::array<Config, 2> configs{};
    std::array<u32, 2> levels{};
    // The sensor: when its last frame was taken, when the next is due, and how many frames the
    // title had presented when it was taken.
    u64 frame_time_us{}, next_frame_us{}, frame_guest_frames{};
    std::array<ExposureGain, 2> exposure{DefaultExposureGain, DefaultExposureGain};

    template <class T>
    bool Read(u64 address, T& value) {
        return address && bool(space.ReadData(GuestCpu::GuestAddress{address},
                                              std::as_writable_bytes(std::span{&value, 1})));
    }
    s32 CheckHandle(s32 value) const {
        if (value < 1)
            return ORBIS_CAMERA_ERROR_PARAM;
        return handle && value == handle ? 0 : ORBIS_CAMERA_ERROR_NOT_OPEN;
    }
    static s32 Normalize(Config& c) {
        using namespace Libraries::Camera;
        if (c.pBaseOption)
            return ORBIS_CAMERA_ERROR_PARAM;
        switch (c.resolution) {
        case ORBIS_CAMERA_RESOLUTION_1280X800:
            c.width = 1280;
            c.height = 800;
            break;
        case ORBIS_CAMERA_RESOLUTION_640X400:
            c.width = 640;
            c.height = 400;
            break;
        case ORBIS_CAMERA_RESOLUTION_320X200:
            c.width = 320;
            c.height = 200;
            break;
        case ORBIS_CAMERA_RESOLUTION_160X100:
            c.width = 160;
            c.height = 100;
            break;
        case ORBIS_CAMERA_RESOLUTION_320X192:
            c.width = 320;
            c.height = 192;
            break;
        case ORBIS_CAMERA_RESOLUTION_SPECIFIED_WIDTH_HEIGHT:
            if (!c.width || !c.height || c.width > 1280 || c.height > 800 || c.width % 2)
                return ORBIS_CAMERA_ERROR_RESOLUTION_UNKNOWN;
            break;
        default:
            return ORBIS_CAMERA_ERROR_RESOLUTION_UNKNOWN;
        }
        if (c.framerate != 7 && c.framerate != 15 && c.framerate != 30 && c.framerate != 60 &&
            c.framerate != 120 && c.framerate != 240)
            return ORBIS_CAMERA_ERROR_BAD_FRAMERATE;
        const auto& f = c.format;
        if (f.formatLevel0 != ORBIS_CAMERA_FORMAT_YUV422 &&
            f.formatLevel0 != ORBIS_CAMERA_FORMAT_RAW16 &&
            f.formatLevel0 != ORBIS_CAMERA_FORMAT_RAW8 &&
            f.formatLevel0 != ORBIS_CAMERA_FORMAT_NO_USE)
            return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
        for (auto value : {f.formatLevel1, f.formatLevel2, f.formatLevel3})
            if (value != ORBIS_CAMERA_SCALE_FORMAT_YUV422 &&
                value != ORBIS_CAMERA_SCALE_FORMAT_Y16 && value != ORBIS_CAMERA_SCALE_FORMAT_Y8 &&
                value != ORBIS_CAMERA_SCALE_FORMAT_NO_USE)
                return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
        return 0;
    }
    bool EnsurePool() {
        if (pool)
            return true;
        const auto address = allocate(MaxPlaneBytes * 2);
        auto pin =
            space.AcquireDataSpan({GuestCpu::GuestAddress{address}, MaxPlaneBytes * 2}, true);
        if (!pin)
            return false;
        auto bytes = pin.Value().WritableBytes();
        // RAW/Y8/Y16 black; YUY2 studio black Y=16, U=V=128. Both immutable
        // slabs can be shared by channels/scaled levels because every pixel is constant.
        std::memset(bytes.data(), 0, MaxPlaneBytes);
        for (u64 i = 0; i < MaxPlaneBytes; i += 2) {
            bytes[MaxPlaneBytes + i] = std::byte{16};
            bytes[MaxPlaneBytes + i + 1] = std::byte{128};
        }
        pool = address;
        return true;
    }

public:
    GuestCamera(GuestCpu::GuestAddressSpace& space, std::function<u64(u64)> allocate, u32 sdk,
                std::function<u64()> guest_frames, std::function<u64()> now_us)
        : space(space), allocate(std::move(allocate)), sdk(sdk),
          guest_frames(std::move(guest_frames)), now_us(std::move(now_us)) {}
    u32 Dispatch(std::string_view nid, const std::array<u64, 6>& a, bool enabled,
                 std::stop_token stop = {}) {
        using namespace Libraries::Camera;
        using namespace GuestCpu;
        if (nid == "mxgMmR+1Kr0")
            return GetFrameData(a, stop);
        std::unique_lock lock(mutex);
        const s32 arg_handle = s32(a[0]);
        if (nid == "RHYJ7GKOSMg") {
            OrbisCameraGetCalibrationDataParameter p{};
            if (!Read(a[0], p) || !a[1] || p.size != sizeof(p) || p.format_type != 0 ||
                p.function_type < ORBIS_CAMERA_CALIBRATION_DATA_FUNCTION_TYPE_IMAGE_RECTIFICATION ||
                p.function_type >
                    ORBIS_CAMERA_CALIBRATION_DATA_FUNCTION_TYPE_IMAGE_INVERSE_RECTIFICATION)
                return ORBIS_CAMERA_ERROR_PARAM;
            if (!enabled)
                return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
            // An empty rectification mesh: nothing is ever read back out of the blank frames.
            OrbisCameraCalibrationData data{};
            data.format_type = p.format_type;
            data.function_type = p.function_type;
            lock.unlock();
            return space.WriteData(GuestAddress{a[1]}, std::as_bytes(std::span{&data, 1}))
                       ? 0
                       : u32(ORBIS_CAMERA_ERROR_COPYOUT_FAILED);
        }
        if (nid == "p6n3Npi3YY4")
            return s32(a[0]) == 0 ? u32(enabled) : u32(ORBIS_CAMERA_ERROR_PARAM);
        if (nid == "BHn83xrF92E") {
            if (s32(a[0]) != Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_SYSTEM || a[1] ||
                a[2])
                return ORBIS_CAMERA_ERROR_PARAM;
            if (a[3]) {
                OrbisCameraOpenParameter p{};
                if (!Read(a[3], p) || p.sizeThis != sizeof(p))
                    return ORBIS_CAMERA_ERROR_PARAM;
            }
            if (!enabled)
                return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
            if (handle)
                return ORBIS_CAMERA_ERROR_ALREADY_OPEN;
            if (next_handle == INT32_MAX)
                return ORBIS_CAMERA_ERROR_MAX_HANDLE;
            if (!EnsurePool())
                return ORBIS_CAMERA_ERROR_COPYOUT_FAILED;
            handle = next_handle++;
            started = false;
            sequence = 0;
            levels = {};
            configs = {camera_config_types[0][0], camera_config_types[0][1]};
            for (auto& c : configs)
                (void)Normalize(c);
            return u32(handle);
        }
        if (auto error = CheckHandle(arg_handle))
            return u32(error);
        if (nid == "OMS9LlcrvBo" || nid == "ztqH5qNTpTk") {
            handle = 0;
            started = false;
            levels = {};
            exposure = {DefaultExposureGain, DefaultExposureGain};
            wake.notify_all();
            return 0;
        }
        if (nid == "2G2C0nmd++M" || nid == "+X1Kgnn3bzg") {
            started = false;
            wake.notify_all();
            return 0;
        }
        // The channel is a mask of the two sensors.
        const u32 channel = u32(a[1]);
        if (nid == "wgBMXJJA6K4") {
            ExposureGain value{};
            if (channel < ORBIS_CAMERA_CHANNEL_0 || channel > ORBIS_CAMERA_CHANNEL_BOTH ||
                !Read(a[2], value) || a[3])
                return ORBIS_CAMERA_ERROR_PARAM;
            if (!enabled)
                return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
            for (unsigned device = 0; device < 2; ++device)
                if (channel & (1u << device))
                    exposure[device] = value;
            return 0;
        }
        if (nid == "ObIste7hqdk") {
            if (channel < ORBIS_CAMERA_CHANNEL_0 || channel >= ORBIS_CAMERA_CHANNEL_BOTH || !a[2] ||
                a[3])
                return ORBIS_CAMERA_ERROR_PARAM;
            if (!enabled)
                return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
            const auto value = exposure[channel - 1];
            lock.unlock();
            return space.WriteData(GuestAddress{a[2]}, std::as_bytes(std::span{&value, 1}))
                       ? 0
                       : u32(ORBIS_CAMERA_ERROR_COPYOUT_FAILED);
        }
        if (nid == "zIKL4kZleuc") {
            if (channel < ORBIS_CAMERA_CHANNEL_0 || channel > ORBIS_CAMERA_CHANNEL_BOTH || a[3])
                return ORBIS_CAMERA_ERROR_PARAM;
            // Accepted; the virtual sensor's frames are black either way.
            return enabled ? 0 : u32(ORBIS_CAMERA_ERROR_NOT_CONNECTED);
        }
        if (!enabled)
            return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
        if (nid == "VQ+5kAqsE2Q" || nid == "9+SNhbctk64") {
            OrbisCameraConfig p{};
            if (!Read(a[1], p) || p.sizeThis != sizeof(p))
                return ORBIS_CAMERA_ERROR_PARAM;
            if (started)
                return ORBIS_CAMERA_ERROR_BUSY;
            std::array<Config, 2> next;
            if (p.configType >= ORBIS_CAMERA_CONFIG_TYPE1 &&
                p.configType <= ORBIS_CAMERA_CONFIG_TYPE5) {
                if (p.configType == ORBIS_CAMERA_CONFIG_TYPE5 && sdk < 0x04500000)
                    return ORBIS_CAMERA_ERROR_UNKNOWN_CONFIG;
                next = {camera_config_types[p.configType - 1][0],
                        camera_config_types[p.configType - 1][1]};
            } else if (p.configType == ORBIS_CAMERA_CONFIG_EXTENTION) {
                next = {p.configExtention[0], p.configExtention[1]};
            } else
                return ORBIS_CAMERA_ERROR_PARAM;
            for (auto& c : next)
                if (auto error = Normalize(c))
                    return u32(error);
            configs = next;
            return 0;
        }
        if (nid == "wpeyFwJ+UEI" || nid == "8WtmqmE4edw") {
            OrbisCameraVideoSyncParameter p{};
            if (!Read(a[1], p) || p.sizeThis != sizeof(p) || p.videoSyncMode > 1 || p.pModeOption)
                return ORBIS_CAMERA_ERROR_PARAM;
            return 0;
        }
        if (nid == "9EpRYMy7rHU" || nid == "cLxF1QtHch0") {
            OrbisCameraStartParameter p{};
            if (!Read(a[1], p) || p.sizeThis != sizeof(p) || p.pStartOption)
                return ORBIS_CAMERA_ERROR_PARAM;
            if (p.formatLevel[0] > 15 || p.formatLevel[1] > 15 ||
                !(p.formatLevel[0] | p.formatLevel[1]))
                return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
            if (started)
                return ORBIS_CAMERA_ERROR_ALREADY_START;
            for (unsigned device = 0; device < 2; ++device) {
                const auto& f = configs[device].format;
                const std::array<u32, 4> formats{u32(f.formatLevel0), u32(f.formatLevel1),
                                                 u32(f.formatLevel2), u32(f.formatLevel3)};
                for (unsigned level = 0; level < 4; ++level)
                    if ((p.formatLevel[device] & (1u << level)) && formats[level] == 16)
                        return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
            }
            levels = {p.formatLevel[0], p.formatLevel[1]};
            started = true;
            frame_time_us = next_frame_us = frame_guest_frames = 0;
            sequence = 0;
            return 0;
        }
        if (nid == "U3BVwQl2R5Q") {
            u32 size{};
            if (!Read(a[1], size) || (size != OldFrameSize && size != sizeof(Frame)))
                return ORBIS_CAMERA_ERROR_PARAM;
            Frame frame{};
            if (!space.ReadData(GuestAddress{a[1]},
                                std::as_writable_bytes(std::span{&frame, 1}).first(size)))
                return ORBIS_CAMERA_ERROR_COPYOUT_FAILED;
            return !frame.status[0] && !frame.status[1];
        }
        return ORBIS_CAMERA_ERROR_NOT_SUPPORTED;
    }

private:
    static constexpr u32 OldFrameSize = offsetof(Frame, pFramePointerListGarlic);
    static_assert(sizeof(Frame) == 584 && OldFrameSize == 520);
    // The read mode bit that asks for the frame after the one read last.
    static constexpr u32 ReadModeWaitNextFrame = 1;

    u32 GetFrameData(const std::array<u64, 6>& a, std::stop_token stop) {
        using namespace Libraries::Camera;
        using namespace GuestCpu;
        u32 size{};
        if (!Read(a[1], size) || (size != OldFrameSize && size != sizeof(Frame)))
            return ORBIS_CAMERA_ERROR_PARAM;
        u32 read_mode{};
        if (!Read(a[1] + offsetof(Frame, readMode), read_mode))
            return ORBIS_CAMERA_ERROR_COPYOUT_FAILED;
        std::unique_lock lock(mutex);
        if (auto error = CheckHandle(s32(a[0])))
            return u32(error);
        if (!started)
            return ORBIS_CAMERA_ERROR_NOT_START;
        if ((read_mode & ReadModeWaitNextFrame) || sequence == 0) {
            // Next frame: not before its time, and not before the title presented again.
            const s32 opened = handle;
            const auto still_running = [&] { return handle == opened && started; };
            const u64 now = now_us();
            if (next_frame_us > now &&
                wake.wait_for(lock, stop, std::chrono::microseconds(next_frame_us - now),
                              [&] { return !still_running(); }))
                return handle == opened ? u32(ORBIS_CAMERA_ERROR_NOT_START)
                                        : u32(ORBIS_CAMERA_ERROR_NOT_OPEN);
            const u64 target = frame_guest_frames + 1;
            const auto deadline = std::chrono::steady_clock::now() + GuestFrameTimeout;
            u64 presented = guest_frames();
            while (presented < target && !stop.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                if (wake.wait_for(lock, stop, std::chrono::milliseconds{1},
                                  [&] { return !still_running(); }))
                    return handle == opened ? u32(ORBIS_CAMERA_ERROR_NOT_START)
                                            : u32(ORBIS_CAMERA_ERROR_NOT_OPEN);
                presented = guest_frames();
            }
            if (stop.stop_requested() || !still_running())
                return ORBIS_CAMERA_ERROR_NOT_START;
            frame_guest_frames = presented;
            frame_time_us = now_us();
            next_frame_us = std::max(next_frame_us, frame_time_us) + FramePeriodUs;
            ++sequence;
        }
        if (!space.ValidateRange({GuestAddress{pool}, MaxPlaneBytes * 2}, GuestPermission::Read))
            return ORBIS_CAMERA_ERROR_COPYOUT_FAILED;
        Frame frame{};
        frame.sizeThis = size;
        frame.readMode = read_mode;
        const u64 time_us = frame_time_us;
        {
            for (unsigned device = 0; device < 2; ++device) {
                const auto& c = configs[device];
                const auto& f = c.format;
                const std::array<u32, 4> formats{u32(f.formatLevel0), u32(f.formatLevel1),
                                                 u32(f.formatLevel2), u32(f.formatLevel3)};
                frame.status[device] = levels[device] ? 0 : ~u32{0};
                frame.meta.frame[device] = sequence;
                frame.meta.timestamp[device] = time_us;
                frame.meta.deviceTimestamp[device] = u32(time_us);
                frame.meta.exposureGain[device] = exposure[device];
                for (unsigned level = 0; level < 4; ++level) {
                    frame.meta.format[device][level] = formats[level];
                    if (!(levels[device] & (1u << level)))
                        continue;
                    const u32 width = std::max(1u, c.width >> level),
                              height = std::max(1u, c.height >> level);
                    const u32 bpp = (level == 0 ? formats[level] == ORBIS_CAMERA_FORMAT_RAW8
                                                : formats[level] == ORBIS_CAMERA_SCALE_FORMAT_Y8)
                                        ? 1
                                        : 2;
                    const u64 address = pool + (formats[level] == 0 ? MaxPlaneBytes : 0);
                    frame.framePosition[device][level] = {0, 0, width, height};
                    frame.frameSize[device][level] = width * height * bpp;
                    frame.pFramePointerList[device][level] = reinterpret_cast<void*>(address);
                    frame.pFramePointerListGarlic[device][level] = reinterpret_cast<void*>(address);
                }
            }
            frame.meta.vcounter = sequence;
        }
        lock.unlock();
        return space.WriteData(GuestAddress{a[1]},
                               std::as_bytes(std::span{&frame, 1}).first(size))
                   ? 0
                   : u32(ORBIS_CAMERA_ERROR_COPYOUT_FAILED);
    }
};
} // namespace Core::HostRuntime
