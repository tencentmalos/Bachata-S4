// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include "core/host_runtime/guest_msg_dialog.h"
#include "core/host_runtime/guest_error_dialog.h"
#include "core/host_runtime/guest_save_dialog.h"
#include "core/libraries/system/msgdialog_ui.h"
#include "imgui/renderer/imgui_core.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
namespace MD = Libraries::MsgDialog;
namespace CD = Libraries::CommonDialog;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({{base}, 0x8000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({{base + 0x8000}, 0x4000}, GuestPermission::Read));
    GuestSaveDialog save;
    auto dialog = std::make_shared<GuestMsgDialog>(save.CommonDomain(), std::vector<s32>{1000});
    auto call = [&](std::string_view n, std::array<u64, 6> a = {}) {
        return dialog->Invoke(*space, n, a);
    };
    auto sc = [&](std::string_view n) { return save.Invoke(n, *space, {}); };
    auto code = [](CD::Error e) { return u32(e); };
    auto write = [&](u64 p, const auto& v) {
        CHECK(space->WriteData({p}, std::as_bytes(std::span{&v, 1})));
    };
    for (auto n : GuestMsgDialog::Nids)
        CHECK(GuestMsgDialog::IsNid(n));
    CHECK(!GuestMsgDialog::IsNid("unknown"));
    CHECK(call("lDqxaY1UbEo") == code(CD::Error::NOT_SYSTEM_INITIALIZED));
    CHECK(sc("uoUpLGNkygk") == 0);
    CHECK(sc("uoUpLGNkygk") == code(CD::Error::ALREADY_SYSTEM_INITIALIZED));
    CHECK(call("lDqxaY1UbEo") == 0);
    CHECK(sc("BQ3tey0JmQM") == 1);
    CHECK(call("lDqxaY1UbEo") == code(CD::Error::ALREADY_INITIALIZED));
    CHECK(sc("s9e3+YpRnzw") == code(CD::Error::BUSY));
    CHECK(call("CWVW78Qc3fI") == 1);
    CHECK(call("HTrcDKlFKuM") == code(CD::Error::NOT_RUNNING));
    MD::OrbisParam p{};
    p.size = sizeof(p);
    p.baseParam.size = sizeof(p.baseParam);
    p.baseParam.magic = u32(base + 0xc0d1a109);
    p.userId = 1000;
    p.mode = MD::MsgDialogMode::SYSTEM_MSG;
    p.sysMsgParam = reinterpret_cast<MD::SystemMessageParam*>(base + 512);
    MD::SystemMessageParam system{MD::SystemMessageType::CAMERA_NOT_CONNECTED};
    write(base + 512, system);
    auto open = [&] {
        write(base, p);
        return call("b06Hh0DPEaE", {base});
    };
    for (u64 bad : {u64{0}, u64{1}, base + 0xbfff, UINT64_MAX})
        CHECK(call("b06Hh0DPEaE", {bad}) == code(CD::Error::ARG_NULL));
    p.size = 128;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.size = sizeof(p);
    p.baseParam.magic++;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.baseParam.magic--;
    p.baseParam.reserved[0] = 1;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.baseParam.reserved[0] = 0;
    p.reserved[0] = 1;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.reserved[0] = 0;
    p.userId = 42;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.userId = 1000;
    p.sysMsgParam = reinterpret_cast<MD::SystemMessageParam*>(base + 0xc000 - 8);
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.sysMsgParam = reinterpret_cast<MD::SystemMessageParam*>(base + 512);
    CHECK(call("CWVW78Qc3fI") == 1 && !ImGui::Core::IsGamepadInputCaptured());
    CHECK(open() == 0);
    CHECK(ImGui::Core::IsGamepadInputCaptured());
    auto first = dialog->Read();
    CHECK(first.text == "Camera not connected.");
    CHECK(call("6fIC3XKt2k0") == 2);
    CHECK(!dialog->Respond(first.request + 1, 1));
    CHECK(!dialog->Respond(first.request, 0));
    CHECK(call("Lr8ovHH9l6A", {base + 1024}) == code(CD::Error::NOT_FINISHED));
    CHECK(dialog->Respond(first.request, 1));
    CHECK(!dialog->Respond(first.request, 1));
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    for (u64 bad : {u64{0}, u64{1}, base + 0x8000 - 4, base + 0x8000, UINT64_MAX})
        CHECK(call("Lr8ovHH9l6A", {bad}) == code(CD::Error::ARG_NULL));
    std::array<u8, 48> sentinel;
    sentinel.fill(0xa5);
    write(base + 1024, sentinel);
    CHECK(call("Lr8ovHH9l6A", {base + 1025}) == 0);
    CHECK(space->ReadData({base + 1024}, std::as_writable_bytes(std::span{sentinel})));
    MD::DialogResult result{};
    std::memcpy(&result, sentinel.data() + 1, sizeof(result));
    CHECK(sentinel.front() == 0xa5 && sentinel[45] == 0xa5 && result.buttonId == MD::ButtonId::OK);
    CHECK(u32(result.mode) == 0 && result.result == CD::Result::OK);
    CHECK(std::ranges::all_of(result.reserved, [](char c) { return c == 0; }));
    CHECK(open() == 0);
    CHECK(!dialog->Respond(first.request, 1));
    CHECK(call("HTrcDKlFKuM") == 0);
    CHECK(call("ePw-kqZmelo") == 0);
    CHECK(sc("BQ3tey0JmQM") == 0);
    CHECK(sc("s9e3+YpRnzw") == 0);
    CHECK(call("lDqxaY1UbEo") == code(CD::Error::BUSY));
    CHECK(sc("YuH2FA7azqQ") == 0);
    CHECK(call("lDqxaY1UbEo") == 0);
    MD::UserMessageParam user{};
    user.msg = reinterpret_cast<const char*>(base + 2048);
    user.buttonType = MD::ButtonType::YESNO_FOCUS_NO;
    const char text[] = "Guest message";
    write(base + 2048, text);
    p.mode = MD::MsgDialogMode::USER_MSG;
    p.userMsgParam = reinterpret_cast<MD::UserMessageParam*>(base + 512);
    p.sysMsgParam = nullptr;
    write(base + 512, user);
    CHECK(open() == 0);
    CHECK(dialog->Read().focus_second && dialog->Read().text == text);
    CHECK(dialog->Respond(dialog->Read().request, 2));
    CHECK(call("Lr8ovHH9l6A", {base + 1024}) == 0);
    CHECK(space->ReadData({base + 1024}, std::as_writable_bytes(std::span{&result, 1})));
    CHECK(result.result == CD::Result::OK && result.buttonId == MD::ButtonId::NO);
    for (auto type : {MD::ButtonType::NONE, MD::ButtonType::WAIT, MD::ButtonType::WAIT_CANCEL,
                      MD::ButtonType::OK_CANCEL}) {
        user.buttonType = type;
        write(base + 512, user);
        CHECK(open() == 0);
        const auto snapshot = dialog->Read();
        if (type == MD::ButtonType::WAIT || type == MD::ButtonType::NONE) {
            CHECK(!dialog->Respond(snapshot.request, 1));
            CHECK(call("HTrcDKlFKuM") == 0);
        } else
            CHECK(dialog->Respond(snapshot.request, 0));
    }
    user.buttonType = static_cast<MD::ButtonType>(4);
    write(base + 512, user);
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    user.buttonType = MD::ButtonType::TWO_BUTTONS;
    MD::ButtonsParam buttons{};
    buttons.msg1 = reinterpret_cast<const char*>(base + 2048);
    buttons.msg2 = reinterpret_cast<const char*>(base + 2048);
    user.buttonsParam = reinterpret_cast<MD::ButtonsParam*>(base + 768);
    write(base + 512, user);
    write(base + 768, buttons);
    CHECK(open() == 0);
    CHECK(dialog->Read().first == text && dialog->Read().second == text);
    CHECK(call("HTrcDKlFKuM") == 0);
    buttons.msg2 = reinterpret_cast<const char*>(UINT64_MAX);
    write(base + 768, buttons);
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    std::array<char, 8192> unterminated;
    unterminated.fill('x');
    write(base + 2048, unterminated);
    user.buttonType = MD::ButtonType::OK;
    write(base + 512, user);
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    write(base + 2048, text);
    MD::ProgressBarParam progress{};
    progress.barType = MD::ProgressBarType::PERCENTAGE_CANCEL;
    progress.msg = reinterpret_cast<const char*>(base + 2048);
    write(base + 512, progress);
    p.mode = MD::MsgDialogMode::PROGRESS_BAR;
    p.progBarParam = reinterpret_cast<MD::ProgressBarParam*>(base + 512);
    p.userMsgParam = nullptr;
    CHECK(open() == 0);
    CHECK(call("wTpfglkmv34", {1, 50}) == code(CD::Error::PARAM_INVALID));
    CHECK(call("wTpfglkmv34", {0, 60}) == 0 && dialog->Read().progress == 60);
    CHECK(call("Gc5k1qcK4fs", {0, UINT32_MAX}) == 0 && dialog->Read().progress == 59);
    CHECK(call("Gc5k1qcK4fs", {0, 50}) == 0 && dialog->Read().progress == 100);
    CHECK(call("wTpfglkmv34", {0, UINT32_MAX}) == 0 && dialog->Read().progress == UINT32_MAX);
    CHECK(call("6H-71OdrpXM", {0, UINT64_MAX}) == code(CD::Error::PARAM_INVALID));
    CHECK(call("6H-71OdrpXM", {0, base + 2048}) == 0);
    CHECK(call("HTrcDKlFKuM") == 0);
    CHECK(call("ePw-kqZmelo") == 0);
    CHECK(call("ePw-kqZmelo") == code(CD::Error::NOT_INITIALIZED));
    CHECK(call("lDqxaY1UbEo") == 0);
    p.mode = MD::MsgDialogMode::SYSTEM_MSG;
    write(base + 512, system);
    p.progBarParam = nullptr;
    p.sysMsgParam = reinterpret_cast<MD::SystemMessageParam*>(base + 512);
    CHECK(open() == 0);
    const auto pending = dialog->Read().request;
    std::thread response([&] { dialog->Respond(pending, 1); });
    dialog->Cancel();
    response.join();
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    CHECK(!save.CommonDomain()->IsUsed());
    CHECK(call("lDqxaY1UbEo") == code(CD::Error::INVALID_STATE));

    // Silent acknowledgements use the ordinary result/lease lifecycle without
    // a render frame or input capture. Choice/wait/progress contracts stay live.
    dialog = std::make_shared<GuestMsgDialog>(save.CommonDomain(), std::vector<s32>{1000}, true);
    CHECK(call("lDqxaY1UbEo") == 0);
    p.mode = MD::MsgDialogMode::USER_MSG;
    p.sysMsgParam = nullptr;
    p.userMsgParam = reinterpret_cast<MD::UserMessageParam*>(base + 512);
    user = {};
    user.msg = reinterpret_cast<const char*>(base + 2048);
    user.buttonType = MD::ButtonType::OK;
    write(base + 512, user);
    write(base + 2048, text);
    CHECK(open() == 0);
    CHECK(call("CWVW78Qc3fI") == u32(CD::Status::FINISHED));
    CHECK(!ImGui::Core::IsGamepadInputCaptured() && save.CommonDomain()->IsUsed());
    CHECK(call("Lr8ovHH9l6A", {base + 1024}) == 0);
    CHECK(space->ReadData({base + 1024}, std::as_writable_bytes(std::span{&result, 1})));
    CHECK(result.result == CD::Result::OK && result.buttonId == MD::ButtonId::OK);
    const auto acknowledged = dialog->Read().request;
    CHECK(!dialog->Respond(acknowledged, 1));
    for (auto type : {MD::ButtonType::YESNO, MD::ButtonType::OK_CANCEL,
                      MD::ButtonType::WAIT, MD::ButtonType::NONE}) {
        user.buttonType = type;
        write(base + 512, user);
        CHECK(open() == 0);
        CHECK(call("CWVW78Qc3fI") == u32(CD::Status::RUNNING));
        CHECK(ImGui::Core::IsGamepadInputCaptured());
        CHECK(!dialog->Respond(acknowledged, 1));
        CHECK(call("HTrcDKlFKuM") == 0);
    }
    p.mode = MD::MsgDialogMode::PROGRESS_BAR;
    p.userMsgParam = nullptr;
    p.progBarParam = reinterpret_cast<MD::ProgressBarParam*>(base + 512);
    write(base + 512, progress);
    CHECK(open() == 0 && dialog->Read().status == CD::Status::RUNNING);
    CHECK(call("HTrcDKlFKuM") == 0);
    CHECK(dialog->OpenLocalMessage(1000, "Error code 0x80550006", "Error") == 0);
    CHECK(dialog->Read().status == CD::Status::FINISHED);
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    CHECK(call("ePw-kqZmelo") == 0 && !save.CommonDomain()->IsUsed());
    {
        GuestErrorDialog silent_error(true);
        CHECK(silent_error.Invoke(*space, "I88KChlynSs", {}) == 0);
        Libraries::ErrorDialog::Param error{};
        error.size = sizeof(error);
        error.errorCode = s32(0x80550006);
        write(base + 512, error);
        CHECK(silent_error.Invoke(*space, "M2ZF-ClLhgY", {base + 512}) == 0);
        CHECK(silent_error.Read().status == CD::Status::FINISHED);
        CHECK(silent_error.Read().text.find("0X80550006") != std::string::npos);
        CHECK(!ImGui::Core::IsGamepadInputCaptured());
        CHECK(silent_error.Invoke(*space, "9XAxK2PMwk8", {}) == 0);
    }
    // Restore the interactive UI fixture below.
    p.mode = MD::MsgDialogMode::SYSTEM_MSG;
    p.userMsgParam = nullptr;
    p.progBarParam = nullptr;
    p.sysMsgParam = reinterpret_cast<MD::SystemMessageParam*>(base + 512);
    write(base + 512, system);

    // Real permanent UI layer, including opening-key rejection, response and Stop.
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1920, 1080};
    io.DeltaTime = 1.0f / 60;
    io.Fonts->AddFontDefault();
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&] {
        ImGui::Core::NewFrame();
        ImGui::Render();
    };
    dialog = std::make_shared<GuestMsgDialog>(save.CommonDomain(), std::vector<s32>{1000});
    CHECK(call("lDqxaY1UbEo") == 0);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    CHECK(open() == 0);
    frame();
    frame();
    CHECK(dialog->Read().status == GuestMsgDialog::Status::RUNNING);
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    frame();
    // A direction towards an absent button must not disable the only action.
    io.AddKeyEvent(ImGuiKey_RightArrow, true);
    frame();
    io.AddKeyEvent(ImGuiKey_RightArrow, false);
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    CHECK(dialog->Read().status == GuestMsgDialog::Status::FINISHED);
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    CHECK(open() == 0);
    frame();
    dialog->Cancel();
    dialog.reset();
    frame();
    frame();
    CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    CHECK(!save.CommonDomain()->IsUsed());
    {
        GuestErrorDialog error, other_error;
        auto e = [&](std::string_view nid, std::array<u64, 6> args = {}) {
            return error.Invoke(*space, nid, args);
        };
        CHECK(e("WWiGuh9XfgQ") == u32(CD::Status::NONE));
        CHECK(e("t2FvHRXzgqk") == u32(CD::Status::NONE));
        CHECK(e("M2ZF-ClLhgY", {base}) == code(CD::Error::INVALID_STATE));
        CHECK(e("ekXHb1kDBl0") == code(CD::Error::NOT_RUNNING));
        CHECK(e("9XAxK2PMwk8") == code(CD::Error::NOT_INITIALIZED));
        CHECK(e("I88KChlynSs") == 0);
        CHECK(e("I88KChlynSs") == code(CD::Error::ALREADY_INITIALIZED));
        CHECK(other_error.Invoke(*space, "WWiGuh9XfgQ", {}) == u32(CD::Status::NONE));
        Libraries::ErrorDialog::Param error_param{16, s32(0x804101c8U), -1, 0};
        write(base, error_param);
        CHECK(e("M2ZF-ClLhgY", {0}) == code(CD::Error::ARG_NULL));
        CHECK(e("M2ZF-ClLhgY", {UINT64_MAX - 7}) == code(CD::Error::ARG_NULL));
        error_param.size = 4; write(base, error_param);
        CHECK(e("M2ZF-ClLhgY", {base}) == code(CD::Error::PARAM_INVALID));
        error_param.size = 16; write(base, error_param);
        auto covered_domain = std::make_shared<GuestCommonDialog>();
        covered_domain->Initialize();
        auto covered = std::make_shared<GuestMsgDialog>(covered_domain, std::vector<s32>{1000});
        CHECK(covered->Invoke(*space, "lDqxaY1UbEo", {}) == 0);
        CHECK(covered->OpenLocalMessage(1000, "Covered message") == 0);
        frame(); frame();
        CHECK(e("M2ZF-ClLhgY", {base}) == 0);
        CHECK(e("M2ZF-ClLhgY", {base}) == code(CD::Error::INVALID_STATE));
        CHECK(error.Read().text.find("804101C8") != std::string::npos);
        const auto request = error.Read().request;
        error_param.errorCode = 0; write(base, error_param);
        CHECK(error.Read().text.find("804101C8") != std::string::npos);
        frame(); frame();
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
        io.AddKeyEvent(ImGuiKey_Enter, true); frame();
        CHECK(e("WWiGuh9XfgQ") == u32(CD::Status::FINISHED));
        CHECK(ImGui::Core::IsGamepadInputCaptured()); // covered modal still owns capture
        io.AddKeyEvent(ImGuiKey_Enter, false); frame(); frame();
        CHECK(covered->Read().status == CD::Status::RUNNING);
        io.AddKeyEvent(ImGuiKey_Enter, true); frame();
        CHECK(covered->Read().status == CD::Status::FINISHED);
        CHECK(!ImGui::Core::IsGamepadInputCaptured());
        io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        CHECK(e("M2ZF-ClLhgY", {base}) == 0);
        CHECK(!error.Respond(request));
        CHECK(e("ekXHb1kDBl0") == 0);
        CHECK(e("9XAxK2PMwk8") == 0);
        CHECK(e("I88KChlynSs") == 0);
        CHECK(e("M2ZF-ClLhgY", {base}) == 0);
        CHECK(e("9XAxK2PMwk8") == 0); // desktop allows Terminate while running
        CHECK(!ImGui::Core::IsGamepadInputCaptured());
        CHECK(e("I88KChlynSs") == 0);
        CHECK(e("M2ZF-ClLhgY", {base}) == 0);
        frame(); error.Cancel(); frame();
        CHECK(!ImGui::Core::IsGamepadInputCaptured());
        CHECK(!error.Respond(error.Read().request));
        CHECK(e("I88KChlynSs") == code(CD::Error::INVALID_STATE));
        CHECK(!GuestErrorDialog::IsNid("jrpnVQfJYgQ"));
        CHECK(!GuestErrorDialog::IsNid("wktCiyWoDTI"));
    }
    ImGui::DestroyContext();
    std::printf("MSG_DIALOG checks=%u failures=%u\n", checks, failures);
    return failures != 0;
}
