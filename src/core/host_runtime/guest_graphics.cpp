// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/guest_graphics.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"
#include "core/signals.h"
#include "frontend/window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Core::HostRuntime {
struct GuestGraphics::Impl {
    std::shared_ptr<Frontend::Window> window;
    Platform::IrqController irq;
    Platform::IrqC::Binding irq_binding{irq};
    // PageManager registers here without taking OS signal ownership from FEX
    // and ART. GPU buffer/submit imports remain refused until tracking can use
    // the guest VM transaction and fault-delivery contract; VideoOut creation
    // itself does not protect guest pages or need access-fault delivery.
    SignalDispatch signals{SignalDispatch::Delivery::External};
    Signals::Binding signals_binding{signals};
    std::unique_ptr<Libraries::VideoOut::VideoOutDriver> video;
    explicit Impl(std::shared_ptr<Frontend::Window> window_) : window(std::move(window_)) {
        if (!window || presenter || liverpool)
            throw std::runtime_error("Graphics requires an exclusive runtime and a live window");
        if (!Frontend::BindWindow(window)) throw std::runtime_error("A platform window is already bound");
    }
    void Stop() {
        if (presenter) presenter->RequestStop();
        if (video) video->RequestStop();
    }
    ~Impl() {
        Stop();
        // Keep the VideoOut port alive until the GPU worker has retired too.
        if (video) video->Join();
        liverpool.reset();
        video.reset();
        presenter.reset();
        Frontend::UnbindWindow(window);
    }
};

GuestGraphics::GuestGraphics(std::shared_ptr<Frontend::Window> window,
    std::shared_ptr<const Vulkan::Driver> driver, std::function<u64()> process_time,
    std::function<u64()> tsc, std::function<bool()> splash_visible)
    : impl(std::make_unique<Impl>(std::move(window))) {
    liverpool = std::make_unique<AmdGpu::Liverpool>();
    presenter = std::make_unique<Vulkan::Presenter>(impl->window, liverpool.get(),
                                                   std::move(driver), std::move(splash_visible));
    impl->video = std::make_unique<Libraries::VideoOut::VideoOutDriver>(
        EmulatorSettings.GetInternalScreenWidth(), EmulatorSettings.GetInternalScreenHeight(),
        std::move(process_time), std::move(tsc));
}
GuestGraphics::~GuestGraphics() = default;
void GuestGraphics::RequestStop() { impl->Stop(); }
Libraries::VideoOut::VideoOutDriver& GuestGraphics::VideoOut() { return *impl->video; }
} // namespace Core::HostRuntime
