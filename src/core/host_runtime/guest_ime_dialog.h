// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/ime/ime_dialog.h"

namespace Core::HostRuntime {

// The desktop IME implementation already owns the ImGui panel, virtual
// keyboard and controller/XR-friendly input path. This bridge is the
// production guest boundary: guest structures are copied, nested buffers are
// validated and pinned for the lifetime of the dialog, and only host-owned
// output temporaries are exposed to the desktop calls.
class GuestImeDialog final {
public:
    static constexpr std::array<std::string_view, 15> Nids{
        "oBmw4xrmfKs", "bX4H+sxPI-o", "UFcyYDf+e88", "fy6ntM25pEc", "8jqzzPioYl8",
        "wqsJvRXwl58", "CRD+jSErEJQ", "x01jxu+vxlc", "IADmD4tScBY", "NUeBrN7hzf0",
        "KR6QDasuKco", "oe92cnJQ9HE", "IoKIpNf9EK0", "-2WqB87KKGg", "gyTyVn+bXMw"};

    explicit GuestImeDialog(GuestCpu::GuestAddressSpace& space) : space(space) {}
    GuestImeDialog(const GuestImeDialog&) = delete;
    GuestImeDialog& operator=(const GuestImeDialog&) = delete;

    [[nodiscard]] static bool IsNid(std::string_view nid);
    [[nodiscard]] u64 Invoke(std::string_view nid, const std::array<u64, 6>& args);

    // Called during session cancellation and teardown. It deliberately uses
    // ForceClose, preserving the desktop dialog's cancel semantics and
    // releasing all long-lived guest pins afterwards.
    void Cancel();

private:
    struct Prepared {
        ::OrbisImeDialogParam param{};
        ::OrbisImeParamExtended extended{};
        bool has_extended{};
        std::vector<GuestCpu::PinnedSpan> pins;
    };

    [[nodiscard]] std::optional<Prepared> Prepare(u64 param_address, u64 extended_address,
                                                  bool internal, u64& error);
    [[nodiscard]] bool AddPinRequest(
        std::vector<GuestCpu::GuestAddressSpace::DataRequest>& requests, u64 address, u64 bytes,
        GuestCpu::GuestPermission permission, u64& error) const;
    [[nodiscard]] bool ReadStruct(u64 address, void* output, size_t size) const;
    [[nodiscard]] u64 InvalidAddress() const;
    [[nodiscard]] u64 InvalidExtended() const;
    [[nodiscard]] u64 ErrorCode(::Error error) const {
        return static_cast<u32>(error);
    }

    enum class InitKind { Normal, Internal, Internal2, Internal3 };
    u64 Start(Prepared prepared, InitKind kind, u32 flags = 0, u64 user = 0, u32 resource = 0,
              u32 unk1 = 0, u32 unk2 = 0);
    u64 PanelSize(u64 param_address, u64 extended_address, u64 width_address, u64 height_address,
                  bool extended);
    u64 Result(u64 result_address);
    u64 PositionAndForm(u64 address);
    u64 CurrentStarState(u64 address);

    GuestCpu::GuestAddressSpace& space;
    std::mutex mutex;
    std::optional<Prepared> active;
};

} // namespace Core::HostRuntime
