// SPDX-License-Identifier: GPL-2.0-or-later
#include <map>
#include "core/host_runtime/guest_reprojection.h"
#include "shader_recompiler/resource.h"
#include "core/guest_cpu/api/access_fault.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/emulator_settings.h"
#include "core/host_runtime/guest_graphics.h"
#include "core/host_runtime/guest_data_batch.h"
#include "core/host_runtime/guest_vr_session.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"
#include "core/signals.h"
#include "frontend/window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Core::HostRuntime {
struct GuestGraphics::Impl : Libraries::Kernel::SessionEqueues {
    std::mutex fault_mutex;
    std::exception_ptr fault;
    std::function<void()> fault_notify;
    void Fail(std::exception_ptr error) {
        {
            std::scoped_lock lock(fault_mutex);
            if (!fault)
                fault = error;
        }
        Stop();
        if (fault_notify)
            fault_notify();
    }
    std::mutex queues_mutex;
    std::recursive_mutex submission_mutex;
    struct Queue {
        std::unique_ptr<Libraries::Kernel::EqueueInternal> queue;
        bool deleted{};
    };
    std::map<s64, Queue> queues;
    s64 next_queue{0x40000000};
    Libraries::Kernel::EqueueInternal* Find(s64 handle) override {
        std::scoped_lock lock(queues_mutex);
        auto it = queues.find(handle);
        return it == queues.end() || it->second.deleted ? nullptr : it->second.queue.get();
    }
    std::shared_ptr<Diagnostics::DiagnosticsPublisher> diagnostics{Diagnostics::DiagnosticsHub::Instance().Acquire()};
    std::shared_ptr<Frontend::Window> window;
    Platform::IrqController irq;
    Platform::IrqC::Binding irq_binding{irq};
    // PageManager uses passive dispatch through the parent FEX access-fault
    // adapter. It never replaces the FEX/ART signal handlers; only guest VM
    // pages currently owned by GPU tracking may consume an access fault.
    SignalDispatch signals{SignalDispatch::Delivery::External};
    Signals::Binding signals_binding{signals};
    std::unique_ptr<Libraries::VideoOut::VideoOutDriver> video;
    GuestVrSession vr_session{}; // Prototype only; no provider/frame hooks yet.
    std::unique_ptr<GuestCpu::AccessFaultHandler> faults;
    explicit Impl(std::shared_ptr<Frontend::Window> window_) : window(std::move(window_)) {
        if (!window || presenter || liverpool)
            throw std::runtime_error("Graphics requires an exclusive runtime and a live window");
        if (!Frontend::BindWindow(window)) throw std::runtime_error("A platform window is already bound");
    }
    void Stop() {
        Libraries::GnmDriver::RequestStop();
        if (liverpool)
            liverpool->RequestStop();
        if (presenter)
            presenter->RequestStop();
        if (video)
            video->RequestStop();
    }
    ~Impl() {
        Stop();
        // Keep the VideoOut port alive until the GPU worker has retired too.
        if (video) video->Join();
        liverpool.reset();
        video.reset();
        faults.reset();
        Libraries::VideoOut::BindSessionDriver(nullptr);
        Libraries::GnmDriver::BindEmbeddedShaders({});
        presenter.reset();
        Libraries::Kernel::BindSessionEqueues(nullptr);
        Frontend::UnbindWindow(window);
    }
};

GuestGraphics::GuestGraphics(std::shared_ptr<Frontend::Window> window,
                             std::shared_ptr<const Vulkan::Driver> driver,
                             std::function<u64()> process_time, std::function<u64()> tsc,
                             std::function<bool()> splash_visible, u64 guest_labels,
                             std::function<void()> fault_notify)
    : impl(std::make_unique<Impl>(std::move(window))) {
    impl->fault_notify = std::move(fault_notify);
    if (impl->diagnostics) {
        for (auto signal : {Diagnostics::AdvanceSignal::GuestFlip, Diagnostics::AdvanceSignal::GuestSubmission,
                            Diagnostics::AdvanceSignal::Pm4Consumed, Diagnostics::AdvanceSignal::HostDraw,
                            Diagnostics::AdvanceSignal::QueueSubmit, Diagnostics::AdvanceSignal::HostPresent})
            impl->diagnostics->MarkAvailable(signal, true);
        impl->diagnostics->SetDriverIdentity(driver ? driver->identity : "unavailable");
    }
    liverpool = std::make_unique<AmdGpu::Liverpool>();
    liverpool->fault_handler = [this](std::exception_ptr error) { impl->Fail(error); };
    liverpool->UseOwnedSubmissions();
    presenter = std::make_unique<Vulkan::Presenter>(impl->window, liverpool.get(),
                                                   std::move(driver), std::move(splash_visible));
    impl->video = std::make_unique<Libraries::VideoOut::VideoOutDriver>(
        EmulatorSettings.GetInternalScreenWidth(), EmulatorSettings.GetInternalScreenHeight(),
        std::move(process_time), std::move(tsc), reinterpret_cast<u64*>(guest_labels),
        [this](std::exception_ptr error) { impl->Fail(error); });
    Libraries::VideoOut::BindSessionDriver(impl->video.get());
    Libraries::GnmDriver::InitializeSession();
    Libraries::GnmDriver::BindEmbeddedShaders(
        {guest_labels + 4096, guest_labels + 8192, guest_labels + 12288});
    Libraries::Kernel::BindSessionEqueues(impl.get());
    impl->faults = std::make_unique<GuestCpu::AccessFaultHandler>(
        impl.get(), [](void* owner, void* context, void* address) {
            try {
                return static_cast<Impl*>(owner)->signals.DispatchAccessViolation(context, address);
            } catch (...) {
                return false;
            }
        });
}
GuestGraphics::~GuestGraphics() = default;
std::shared_ptr<Diagnostics::DiagnosticsPublisher> GuestGraphics::DiagnosticsPublisher() const {
    return impl->diagnostics;
}
void GuestGraphics::RequestStop() {
    impl->Stop();
}
void GuestGraphics::CheckHealth() {
    if (presenter) presenter->CheckSubmissionHealth();
    std::scoped_lock lock(impl->fault_mutex);
    if (impl->fault)
        std::rethrow_exception(impl->fault);
}
void GuestGraphics::WaitIdle() {
    CheckHealth();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!liverpool->IsGpuIdle()) {
        CheckHealth();
        if (liverpool->StopRequested() || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("GPU drain cancelled or timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // VM publication / unregister used to observe actual queue submission once
    // Liverpool drained. Keep that boundary after moving submission to a worker.
    // This is not the per-frame GNM admission gate or a GPU-completion wait.
    if (presenter) presenter->DrainSubmissions();
}
std::recursive_mutex& GuestGraphics::SubmissionMutex() {
    return impl->submission_mutex;
}
bool GuestGraphics::Stopping() const {
    return liverpool->StopRequested();
}
s64 GuestGraphics::CreateEqueue(std::string_view name) {
    std::scoped_lock lock(impl->queues_mutex);
    const auto handle = impl->next_queue++;
    impl->queues.emplace(
        handle, Impl::Queue{std::make_unique<Libraries::Kernel::EqueueInternal>(handle, name)});
    return handle;
}
s32 GuestGraphics::DeleteEqueue(s64 handle) {
    std::scoped_lock lock(impl->queues_mutex);
    auto it = impl->queues.find(handle);
    if (it == impl->queues.end() || it->second.deleted)
        return ORBIS_KERNEL_ERROR_EBADF;
    // IRQ handlers may already hold this queue. Retain its allocation until
    // renderer/VideoOut workers have joined, but refuse new guest operations.
    it->second.deleted = true;
    return 0;
}
Libraries::Kernel::EqueueInternal* GuestGraphics::FindEqueue(s64 handle) {
    return impl->Find(handle);
}
Libraries::VideoOut::VideoOutDriver& GuestGraphics::VideoOut() {
    return *impl->video;
}
s32 GuestGraphics::ValidateVrDisplay(s32 video, s32 first, s32 second) {
    auto* port = impl->video->GetPort(video);
    if (!port || !port->is_open) return ORBIS_HMD_ERROR_HANDLE_INVALID;
    std::lock_guard lock(port->port_mutex);
    if (first < 0 || second < 0 || first >= Libraries::VideoOut::MaxDisplayBuffers ||
        second >= Libraries::VideoOut::MaxDisplayBuffers || first == second)
        return ORBIS_HMD_ERROR_REPROJECTION_WRONG_DISPLAY_BUFFER;
    for (const auto index : {first, second}) {
        const auto group = port->buffer_slots[index].group_index;
        if (group < 0) return ORBIS_HMD_ERROR_REPROJECTION_WRONG_DISPLAY_BUFFER;
        const auto& attr = port->groups[group].attrib;
        if (attr.width != 1920 || attr.height != 1080)
            return ORBIS_HMD_ERROR_REPROJECTION_WRONG_DISPLAY_BUFFER;
    }
    return 0;
}

s32 GuestGraphics::SubmitVrFrame(GuestCpu::GuestAddressSpace& space, const ReprojectionFrame& frame,
                                 std::function<void(bool)> complete) {
    using namespace GuestCpu;
    GuestDataBatch batch;
    if (frame.image_count != 2 && frame.image_count != 4) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    for (u32 i = 0; i < frame.image_count; ++i) {
        const auto& eye = frame.eyes[i];
        // Keep malformed or not-yet-supported descriptors out of assert-based
        // texture-cache constructors. Only uncompressed, single-sample 2D eyes.
        const bool format_ok =
            (eye.data_format == u64(AmdGpu::DataFormat::Format8_8_8_8) &&
             (eye.num_format == u64(AmdGpu::NumberFormat::Unorm) || eye.num_format == u64(AmdGpu::NumberFormat::Srgb))) ||
            (eye.data_format == u64(AmdGpu::DataFormat::Format16_16_16_16) &&
             (eye.num_format == u64(AmdGpu::NumberFormat::Unorm) || eye.num_format == u64(AmdGpu::NumberFormat::Float)));
        if (!eye || !format_ok ||
            (eye.num_format != u64(AmdGpu::NumberFormat::Unorm) &&
             eye.num_format != u64(AmdGpu::NumberFormat::Srgb) &&
             eye.num_format != u64(AmdGpu::NumberFormat::Float)) ||
            (eye.GetType() != AmdGpu::ImageType::Color2D && eye.GetType() != AmdGpu::ImageType::Color2DArray) ||
            eye.width >= 4096 || eye.height >= 4096 || eye.Pitch() < eye.width + 1 ||
            eye.base_level || eye.last_level || eye.depth > 1 || eye.last_array > 1 ||
            eye.base_array > eye.last_array || eye.tiling_index > 31 || eye.compression_en)
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        const VideoCore::ImageInfo info{eye, Shader::ImageResource{}};
        if (!info.guest_size || info.guest_size > 128 * 1024 * 1024)
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        if (!batch.Add(space, {{info.guest_address}, info.guest_size}, GuestPermission::Read))
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    if ((frame.completion_label & 7) ||
        !batch.Add(space, {{frame.completion_label}, 8}, GuestPermission::Write))
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    auto acquired = batch.Acquire(space);
    if (!acquired) return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    auto pins = std::make_shared<std::vector<PinnedSpan>>(std::move(acquired).Value());
    auto* port = impl->video->GetPort(frame.video_handle);
    if (!port) return ORBIS_HMD_ERROR_HANDLE_INVALID;
    // The label is firmware's texture-release word, NOT an acceptance receipt.
    // Release it only after the presenter GPU reads and queue submit both retire.
    const bool accepted = impl->video->SubmitVrFrame(port, frame.display_index, frame.sequence,
        frame, [pins, complete = std::move(complete)](bool retired) {
            if (retired && pins->back().OwnerAlive()) {
                auto* label = reinterpret_cast<u64*>(pins->back().WritableBytes().data());
                std::atomic_ref<u64>(*label).store(0, std::memory_order_release);
            }
            pins->clear();
            complete(retired);
        });
    return accepted ? 0 : ORBIS_HMD_ERROR_REPROJECTION_DISPLAY_BUFFER_BUSY;
}

GuestVrSession& GuestGraphics::VrSession() {
    return impl->vr_session;
}
} // namespace Core::HostRuntime
