// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_ime_dialog.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>

#include "core/libraries/error_codes.h"

namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace Libraries::ImeDialog;

namespace {
constexpr u32 kMaxImeStringUnits = 4096;

u64 GuestPointer(const void* pointer) {
    return reinterpret_cast<u64>(pointer);
}

} // namespace

bool GuestImeDialog::IsNid(std::string_view nid) {
    return std::ranges::find(Nids, nid) != Nids.end();
}

u64 GuestImeDialog::InvalidAddress() const {
    return ErrorCode(::Error::INVALID_ADDRESS);
}

u64 GuestImeDialog::InvalidExtended() const {
    return ErrorCode(::Error::INVALID_EXTENDED);
}

bool GuestImeDialog::ReadStruct(u64 address, void* output, size_t size) const {
    if (!address || address > std::numeric_limits<u64>::max() - size)
        return false;
    return bool(
        space.ReadData(GuestAddress{address}, std::span{static_cast<std::byte*>(output), size}));
}

bool GuestImeDialog::AddPinRequest(std::vector<GuestAddressSpace::DataRequest>& requests,
                                   u64 address, u64 bytes, GuestPermission permission,
                                   u64& error) const {
    if (!address || bytes == 0 || address > std::numeric_limits<u64>::max() - bytes) {
        error = InvalidAddress();
        return false;
    }
    const auto checked = GuestRange::Checked(GuestAddress{address}, bytes);
    if (!checked || !space.ValidateRange(checked.Value(), permission)) {
        error = InvalidAddress();
        return false;
    }
    requests.push_back({checked.Value(), permission});
    return true;
}

std::optional<GuestImeDialog::Prepared> GuestImeDialog::Prepare(u64 param_address,
                                                                u64 extended_address, bool internal,
                                                                u64& error) {
    if (!param_address ||
        !space.ValidateRange({GuestAddress{param_address}, sizeof(OrbisImeDialogParam)},
                             GuestPermission::Read)) {
        error = InvalidAddress();
        return std::nullopt;
    }

    OrbisImeDialogParam raw{};
    if (!ReadStruct(param_address, &raw, sizeof(raw))) {
        error = InvalidAddress();
        return std::nullopt;
    }
    if (raw.max_text_length == 0 || raw.max_text_length > ORBIS_IME_MAX_TEXT_LENGTH) {
        error = ErrorCode(::Error::INVALID_MAX_TEXT_LENGTH);
        return std::nullopt;
    }
    const u64 text_address = GuestPointer(raw.input_text_buffer);
    if (!text_address) {
        error = ErrorCode(::Error::INVALID_INPUT_TEXT_BUFFER);
        return std::nullopt;
    }

    OrbisImeParamExtended raw_extended{};
    bool has_extended = false;
    if (extended_address) {
        if (!space.ValidateRange({GuestAddress{extended_address}, sizeof(raw_extended)},
                                 GuestPermission::Read) ||
            !ReadStruct(extended_address, &raw_extended, sizeof(raw_extended))) {
            error = InvalidAddress();
            return std::nullopt;
        }
        // An ext keyboard callback is a guest PC. Calling it from the host
        // ImGui thread would cross the FEX boundary without a guest call
        // context, so retain the retail error rather than installing a raw
        // host callback. The normal OSK/XR path does not need it.
        if (raw_extended.ext_keyboard_filter) {
            error = InvalidExtended();
            return std::nullopt;
        }
        has_extended = true;
    }

    std::vector<GuestAddressSpace::DataRequest> requests;
    requests.reserve(4);
    if (!AddPinRequest(requests, text_address,
                       (static_cast<u64>(raw.max_text_length) + 1) * sizeof(char16_t),
                       GuestPermission::Read | GuestPermission::Write, error))
        return std::nullopt;

    const auto add_text = [&](u64 address) {
        if (!address)
            return true;
        return AddPinRequest(requests, address,
                             (static_cast<u64>(kMaxImeStringUnits) + 1) * sizeof(char16_t),
                             GuestPermission::Read, error);
    };
    if (!add_text(GuestPointer(raw.placeholder)) || !add_text(GuestPointer(raw.title)))
        return std::nullopt;

    if (has_extended && raw_extended.additional_dictionary_path) {
        if (!AddPinRequest(requests, GuestPointer(raw_extended.additional_dictionary_path),
                           kMaxImeStringUnits + 1, GuestPermission::Read, error))
            return std::nullopt;
    }

    auto pins = space.AcquireDataBatch(requests);
    if (!pins) {
        error = InvalidAddress();
        return std::nullopt;
    }

    // Title and placeholder are consumed with char_traits::length by the
    // shared ImGui state. Require a terminator inside the bounded pinned view
    // before handing them to desktop code.
    size_t text_index = 1;
    const auto check_utf16 = [&](size_t index) {
        const auto bytes = pins.Value()[index].Bytes();
        const auto* chars = reinterpret_cast<const char16_t*>(bytes.data());
        return std::ranges::find(chars, chars + kMaxImeStringUnits + 1, u'\0') !=
               chars + kMaxImeStringUnits + 1;
    };
    if (raw.placeholder && !check_utf16(text_index++)) {
        error = InvalidAddress();
        return std::nullopt;
    }
    if (raw.title && !check_utf16(text_index++)) {
        error = InvalidAddress();
        return std::nullopt;
    }

    Prepared prepared;
    prepared.param = raw;
    prepared.extended = raw_extended;
    prepared.has_extended = has_extended;
    prepared.pins = std::move(pins).Value();
    (void)internal; // The desktop functions perform the SDK-specific checks.
    return std::optional<Prepared>(std::move(prepared));
}

u64 GuestImeDialog::Start(Prepared prepared, InitKind kind, u32 flags, u64 user, u32 resource,
                          u32 unk1, u32 unk2) {
    // Keep the pin vector installed before calling into SetupDialogState: the
    // shared ImeDialogState validates and reads the nested guest buffers.
    active.emplace(std::move(prepared));
    auto& state = *active;
    const auto extended = state.has_extended ? &state.extended : nullptr;
    int result{};
    switch (kind) {
    case InitKind::Normal:
        result = static_cast<int>(
            sceImeDialogInit(&state.param, const_cast<OrbisImeParamExtended*>(extended)));
        break;
    case InitKind::Internal:
        result =
            sceImeDialogInitInternal(&state.param, const_cast<OrbisImeParamExtended*>(extended));
        break;
    case InitKind::Internal2:
        result = sceImeDialogInitInternal2(reinterpret_cast<int*>(&state.param),
                                           const_cast<u32*>(reinterpret_cast<const u32*>(extended)),
                                           flags, user);
        break;
    case InitKind::Internal3:
        result = sceImeDialogInitInternal3(reinterpret_cast<int*>(&state.param),
                                           const_cast<u32*>(reinterpret_cast<const u32*>(extended)),
                                           flags, user, resource, unk2);
        break;
    }
    if (result != 0)
        active.reset();
    return static_cast<u32>(result);
}

u64 GuestImeDialog::PanelSize(u64 param_address, u64 extended_address, u64 width_address,
                              u64 height_address, bool extended) {
    OrbisImeDialogParam param{};
    if (!ReadStruct(param_address, &param, sizeof(param)))
        return InvalidAddress();
    OrbisImeParamExtended ext{};
    if (extended && !ReadStruct(extended_address, &ext, sizeof(ext)))
        return InvalidAddress();
    auto width = space.AcquireDataSpan({GuestAddress{width_address}, sizeof(u32)}, true);
    auto height = space.AcquireDataSpan({GuestAddress{height_address}, sizeof(u32)}, true);
    if (!width || !height)
        return InvalidAddress();
    u32 width_value{}, height_value{};
    const auto result =
        extended ? sceImeDialogGetPanelSizeExtended(&param, &ext, &width_value, &height_value)
                 : sceImeDialogGetPanelSize(&param, &width_value, &height_value);
    if (result == ::Error::OK) {
        std::memcpy(width.Value().WritableBytes().data(), &width_value, sizeof(width_value));
        std::memcpy(height.Value().WritableBytes().data(), &height_value, sizeof(height_value));
    }
    return ErrorCode(result);
}

u64 GuestImeDialog::Result(u64 result_address) {
    auto pin =
        space.AcquireDataSpan({GuestAddress{result_address}, sizeof(OrbisImeDialogResult)}, true);
    if (!pin)
        return InvalidAddress();
    OrbisImeDialogResult result{};
    std::memcpy(&result, pin.Value().Bytes().data(), sizeof(result));
    const auto status = sceImeDialogGetResult(&result);
    if (status == ::Error::OK)
        std::memcpy(pin.Value().WritableBytes().data(), &result, sizeof(result));
    return ErrorCode(status);
}

u64 GuestImeDialog::PositionAndForm(u64 address) {
    auto pin =
        space.AcquireDataSpan({GuestAddress{address}, sizeof(OrbisImePositionAndForm)}, true);
    if (!pin)
        return InvalidAddress();
    OrbisImePositionAndForm value{};
    const auto status = sceImeDialogGetPanelPositionAndForm(&value);
    if (status == 0)
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
    return static_cast<u32>(status);
}

u64 GuestImeDialog::CurrentStarState(u64 address) {
    auto pin = space.AcquireDataSpan({GuestAddress{address}, sizeof(u32)}, true);
    if (!pin)
        return InvalidAddress();
    u32 state{};
    const auto status = sceImeDialogGetCurrentStarState(reinterpret_cast<s64>(&state));
    if (status == 0)
        std::memcpy(pin.Value().WritableBytes().data(), &state, sizeof(state));
    return static_cast<u32>(status);
}

u64 GuestImeDialog::Invoke(std::string_view nid, const std::array<u64, 6>& args) {
    std::scoped_lock lock(mutex);
    if (nid == "oBmw4xrmfKs")
        return ErrorCode(sceImeDialogAbort());
    if (nid == "bX4H+sxPI-o") {
        const auto result = sceImeDialogForceClose();
        active.reset();
        return ErrorCode(result);
    }
    if (nid == "UFcyYDf+e88")
        return ErrorCode(sceImeDialogForTestFunction());
    if (nid == "IADmD4tScBY")
        return static_cast<u32>(sceImeDialogGetStatus());
    if (nid == "fy6ntM25pEc")
        return CurrentStarState(args[0]);
    if (nid == "8jqzzPioYl8")
        return PositionAndForm(args[0]);
    if (nid == "wqsJvRXwl58")
        return PanelSize(args[0], 0, args[1], args[2], false);
    if (nid == "CRD+jSErEJQ")
        return PanelSize(args[0], args[1], args[2], args[3], true);
    if (nid == "x01jxu+vxlc")
        return Result(args[0]);
    if (nid == "NUeBrN7hzf0" || nid == "KR6QDasuKco" || nid == "oe92cnJQ9HE" ||
        nid == "IoKIpNf9EK0") {
        u64 error{};
        const bool internal = nid != "NUeBrN7hzf0";
        auto prepared = Prepare(args[0], args[1], internal, error);
        if (!prepared)
            return error;
        if (nid == "NUeBrN7hzf0")
            return Start(std::move(*prepared), InitKind::Normal);
        if (nid == "KR6QDasuKco")
            return Start(std::move(*prepared), InitKind::Internal);
        if (nid == "oe92cnJQ9HE")
            return Start(std::move(*prepared), InitKind::Internal2, u32(args[2]), args[3]);
        return Start(std::move(*prepared), InitKind::Internal3, u32(args[2]), args[3], u32(args[4]),
                     0, u32(args[5]));
    }
    if (nid == "-2WqB87KKGg")
        return static_cast<u32>(
            sceImeDialogSetPanelPosition(static_cast<s32>(args[0]), static_cast<s32>(args[1])));
    if (nid == "gyTyVn+bXMw") {
        const auto result = sceImeDialogTerm();
        if (result == ::Error::OK)
            active.reset();
        return ErrorCode(result);
    }
    return ErrorCode(::Error::INVALID_ARG);
}

void GuestImeDialog::Cancel() {
    std::scoped_lock lock(mutex);
    if (!active)
        return;
    (void)sceImeDialogForceClose();
    active.reset();
}

} // namespace Core::HostRuntime
