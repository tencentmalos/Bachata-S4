// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Typed guest -> host HLE call adapter for the x86-64 SysV ABI.
//
// Migrated in shape from references/Bachata-S4-android
// src/core/guest_cpu/hle_call_adapter.h (pinned 67dbf4e5), with three
// deliberate changes required by the V0 API contract §8:
//
//   1. Unsupported signatures are refused at *registration* time, not on the
//      first call. The reference decided per-invocation, which means a
//      mis-registered function looks fine until a guest reaches it.
//   2. Guest pointers are validated against the owning GuestAddressSpace. The
//      reference let a non-null unvalidated pointer through when it was above
//      4096, which hands an unchecked host address to native code.
//   3. Errors are Result/Error, so a refusal says which argument and why.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"

namespace Core::GuestCpu::Hle {

// The x86-64 SysV integer argument order, as indices into the public GPR
// numbering: rdi, rsi, rdx, rcx, r8, r9.
//
// Note rcx is the 4th. A syscall-style callgate clobbers rcx and passes the
// 4th argument in r10, so whoever builds the frame must have already
// normalised that -- acceptance H01 checks precisely this.
inline constexpr std::array<Gpr, 6> kSysVIntegerOrder{
    Gpr::Rdi, Gpr::Rsi, Gpr::Rdx, Gpr::Rcx, Gpr::R8, Gpr::R9,
};
inline constexpr std::size_t kSysVVectorRegisters = 8;  // xmm0..xmm7

// Guest register state at the HLE boundary, plus the address space needed to
// validate any pointer the guest passed.
struct HleCallFrame final {
    std::uint64_t operation{};
    RegisterFile registers{};
    GuestAddressSpace* space{};
    // Set when the frame came through a syscall-style gate, meaning the 4th
    // integer argument arrived in r10 and has been copied into rcx.
    bool rcx_normalised_from_r10{false};

    // Guest pointer arguments are pinned for the duration of the native call so a concurrent
    // unmap/remap/protect on another owner cannot retire or change the backing while native code
    // is reading or writing it. Released when the frame is destroyed, on every path.
    std::vector<PinnedSpan> pins{};
};

// Walks arguments in SysV order, spilling to the guest stack once the
// registers are exhausted.
class CallCursor final {
public:
    explicit CallCursor(HleCallFrame& frame_) : frame{frame_} {}

    [[nodiscard]] Result<std::uint64_t> NextInteger() {
        if (integer_index < kSysVIntegerOrder.size()) {
            return frame.registers.Get(kSysVIntegerOrder[integer_index++]);
        }
        return NextStackSlot();
    }

    [[nodiscard]] Result<std::uint64_t> NextVector() {
        if (vector_index < kSysVVectorRegisters) {
            return frame.registers.xmm[vector_index++].low;
        }
        return NextStackSlot();
    }

    [[nodiscard]] std::size_t IntegerArgumentsUsed() const noexcept {
        return integer_index;
    }
    [[nodiscard]] std::size_t VectorArgumentsUsed() const noexcept {
        return vector_index;
    }

private:
    // Stack arguments begin above the return address pushed by the call.
    [[nodiscard]] Result<std::uint64_t> NextStackSlot() {
        if (frame.space == nullptr) {
            return MakeError(ErrorCategory::InvalidArgument, "CallCursor::NextStackSlot",
                             "frame has no address space, cannot read spilled arguments");
        }
        const std::uint64_t rsp = frame.registers.Rsp();
        if (rsp == 0) {
            return MakeError(ErrorCategory::InvalidArgument, "CallCursor::NextStackSlot",
                             "guest RSP is null");
        }
        if (rsp > UINT64_MAX - stack_offset) {
            return MakeError(ErrorCategory::InvalidArgument, "CallCursor::NextStackSlot",
                             "guest stack offset overflows");
        }
        const GuestAddress at{rsp + stack_offset};
        std::uint64_t value{};
        std::array<std::byte, sizeof(std::uint64_t)> bytes{};
        if (auto status = frame.space->Read(at, bytes); !status) {
            return status.GetError();
        }
        std::memcpy(&value, bytes.data(), sizeof(value));
        stack_offset += sizeof(std::uint64_t);
        return value;
    }

    HleCallFrame& frame;
    std::size_t integer_index{};
    std::size_t vector_index{};
    // Skip the return address.
    std::uint64_t stack_offset{sizeof(std::uint64_t)};
};

// A bounded guest buffer: a guest pointer followed (in the integer argument stream) by an element
// count. The native function receives a host pointer (the DirectMapped reservation address) that is
// pinned for count*sizeof(T) bytes across the whole call, so an oversized or unbounded buffer is
// refused before entry rather than letting native code walk off the mapping. The host side sees
// {data, count}.
template <typename T>
struct GuestBoundedBuffer {
    T* data{};
    std::uint64_t count{};
};

namespace detail {

template <typename T>
inline constexpr bool IsGuestPointer = std::is_pointer_v<T>;

// Written as a lambda so `sizeof(T)` is never instantiated for void, which is
// an incomplete type.
template <typename T>
inline constexpr bool IsIntegerArgument = [] {
    if constexpr (std::is_void_v<T>) {
        return false;
    } else {
        return (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= sizeof(std::uint64_t);
    }
}();

template <typename T>
inline constexpr bool IsVectorArgument = std::is_same_v<T, float> || std::is_same_v<T, double>;

// Detects a GuestBoundedBuffer<U>.
template <typename T>
inline constexpr bool IsBoundedBuffer = false;
template <typename U>
inline constexpr bool IsBoundedBuffer<GuestBoundedBuffer<U>> = true;

// V0 accepts scalars, validated guest pointers and bounded buffers. Aggregates
// and varargs are refused at registration (§8).
template <typename T>
inline constexpr bool IsSupportedArgument =
    IsIntegerArgument<T> || IsVectorArgument<T> || IsGuestPointer<T> || IsBoundedBuffer<T>;

template <typename T>
inline constexpr bool IsSupportedReturn =
    std::is_void_v<T> || IsIntegerArgument<T> || IsVectorArgument<T> || IsGuestPointer<T>;

// A guest pointer argument is only handed to native code after its range and
// permissions have been checked in the guest address space.
template <typename T>
Result<T> DecodePointer(CallCursor& cursor, HleCallFrame& frame) {
    auto raw = cursor.NextInteger();
    if (!raw) {
        return raw.GetError();
    }
    if (raw.Value() == 0) {
        return static_cast<T>(nullptr);
    }
    if (frame.space == nullptr) {
        return MakeError(ErrorCategory::InvalidArgument, "Hle::DecodePointer",
                         "cannot validate a guest pointer without an address space");
    }

    using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
    constexpr std::size_t needed = [] {
        if constexpr (std::is_void_v<Pointee>) {
            return std::size_t{1};
        } else {
            return sizeof(Pointee);
        }
    }();
    constexpr bool writable = !std::is_const_v<std::remove_pointer_t<T>>;

    auto range = GuestRange::Checked(GuestAddress{raw.Value()}, needed);
    if (!range) {
        return range.GetError();
    }

    // Pin the span for the whole native call, not just a one-shot permission check: a concurrent
    // remap/unmap on another owner must not retire the backing while native code holds the pointer.
    // The pin is owned by the frame and released on return (success or failure).
    auto pin = frame.space->AcquirePinnedSpan(range.Value(), writable);
    if (!pin) {
        auto error = pin.GetError();
        error.operation = "Hle::DecodePointer";
        return error;
    }
    frame.pins.push_back(std::move(pin.Value()));
    return reinterpret_cast<T>(static_cast<std::uintptr_t>(raw.Value()));
}

template <typename T>
Result<T> DecodeArgument(CallCursor& cursor, HleCallFrame& frame) {
    if constexpr (IsVectorArgument<T>) {
        auto bits = cursor.NextVector();
        if (!bits) {
            return bits.GetError();
        }
        T value{};
        // float occupies the low 32 bits of the xmm register.
        std::memcpy(&value, &bits.Value(), sizeof(value));
        return value;
    } else if constexpr (IsBoundedBuffer<T>) {
        // Two integer args: the guest pointer, then the element count. Pin count*sizeof(element) so
        // native code can walk exactly that many; an oversized buffer is refused before entry.
        using ElemType = std::remove_pointer_t<decltype(T{}.data)>;
        auto raw_ptr = cursor.NextInteger();
        auto raw_count = cursor.NextInteger();
        if (!raw_ptr) return raw_ptr.GetError();
        if (!raw_count) return raw_count.GetError();
        T buffer{};
        buffer.count = raw_count.Value();
        if (raw_ptr.Value() == 0) {
            if (buffer.count != 0) {
                return MakeError(ErrorCategory::InvalidArgument, "Hle::DecodeBuffer",
                                 "null buffer with non-zero length");
            }
            return buffer;
        }
        if (frame.space == nullptr) {
            return MakeError(ErrorCategory::InvalidArgument, "Hle::DecodeBuffer",
                             "cannot validate a guest buffer without an address space");
        }
        constexpr std::uint64_t elem_size = sizeof(ElemType);
        if (raw_count.Value() != 0 &&
            raw_count.Value() > (UINT64_MAX - raw_ptr.Value()) / elem_size) {
            return MakeError(ErrorCategory::InvalidArgument, "Hle::DecodeBuffer",
                             "buffer byte length overflows");
        }
        const std::uint64_t bytes = raw_count.Value() * elem_size;
        auto range = GuestRange::Checked(GuestAddress{raw_ptr.Value()}, bytes ? bytes : 1);
        if (!range) return range.GetError();
        auto pin = frame.space->AcquirePinnedSpan(
            range.Value(), /*writable=*/!std::is_const_v<ElemType>);
        if (!pin) return pin.GetError();
        buffer.data = reinterpret_cast<ElemType*>(static_cast<std::uintptr_t>(raw_ptr.Value()));
        frame.pins.push_back(std::move(pin.Value()));
        return buffer;
    } else if constexpr (IsGuestPointer<T>) {
        return DecodePointer<T>(cursor, frame);
    } else {
        auto raw = cursor.NextInteger();
        if (!raw) {
            return raw.GetError();
        }
        return static_cast<T>(raw.Value());
    }
}

template <typename T>
void EncodeReturn(HleCallFrame& frame, const T& value) {
    if constexpr (IsVectorArgument<T>) {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(value));
        frame.registers.xmm[0] = Xmm{bits, 0};
        frame.registers.validity |= RegisterValidity::Xmm;
    } else if constexpr (IsGuestPointer<T>) {
        frame.registers.Set(Gpr::Rax, reinterpret_cast<std::uintptr_t>(value));
        frame.registers.validity |= RegisterValidity::Gpr;
    } else {
        frame.registers.Set(Gpr::Rax, static_cast<std::uint64_t>(value));
        frame.registers.validity |= RegisterValidity::Gpr;
    }
}

} // namespace detail

// Registered HLE entry point.
class HleCallAdapter {
public:
    virtual ~HleCallAdapter() = default;

    [[nodiscard]] std::uint64_t Operation() const noexcept { return operation; }
    [[nodiscard]] const std::string& Name() const noexcept { return name; }

    virtual Status Invoke(HleCallFrame& frame) const = 0;
    // Signature support is a compile-time property, surfaced so the registry
    // can refuse at registration rather than at first call.
    [[nodiscard]] virtual bool SignatureSupported() const noexcept = 0;
    [[nodiscard]] virtual std::string SignatureDescription() const = 0;

private:
    friend class HleCallRegistry;
    void Assign(std::uint64_t operation_, std::string name_) {
        operation = operation_;
        name = std::move(name_);
    }
    std::uint64_t operation{};
    std::string name;
};

template <typename Function>
class TypedHleCallAdapter;

template <typename Return, typename... Args>
class TypedHleCallAdapter<Return (*)(Args...)> final : public HleCallAdapter {
public:
    explicit TypedHleCallAdapter(Return (*function_)(Args...)) : function{function_} {}

    [[nodiscard]] bool SignatureSupported() const noexcept override {
        return kSupported;
    }

    [[nodiscard]] std::string SignatureDescription() const override {
        return kSupported ? "supported scalar/pointer signature"
                          : "unsupported: aggregate, oversized or varargs parameter";
    }

    Status Invoke(HleCallFrame& frame) const override {
        if constexpr (!kSupported) {
            return MakeError(ErrorCategory::Unsupported, "HleCallAdapter::Invoke",
                             "signature was refused at registration");
        } else {
            CallCursor cursor{frame};
            // Decode left to right: SysV argument slot assignment is
            // positional, so the order of these calls is load-bearing.
            std::tuple<Result<Args>...> decoded{detail::DecodeArgument<Args>(cursor, frame)...};

            if (auto failure = FirstFailure(decoded, std::index_sequence_for<Args...>{})) {
                return *failure;
            }
            if constexpr (std::is_void_v<Return>) {
                Apply(decoded, std::index_sequence_for<Args...>{});
                frame.registers.Set(Gpr::Rax, 0);
                frame.registers.validity |= RegisterValidity::Gpr;
            } else {
                detail::EncodeReturn(frame,
                                     Apply(decoded, std::index_sequence_for<Args...>{}));
            }
            return Ok();
        }
    }

private:
    static constexpr bool kSupported =
        (detail::IsSupportedArgument<Args> && ...) && detail::IsSupportedReturn<Return>;

    template <std::size_t... I>
    static std::optional<Error> FirstFailure(const std::tuple<Result<Args>...>& decoded,
                                             std::index_sequence<I...>) {
        std::optional<Error> failure;
        // Report the first bad argument rather than a generic EFAULT, so a
        // caller can tell which parameter was rejected.
        ((failure || (std::get<I>(decoded).HasValue()
                          ? false
                          : (failure = std::get<I>(decoded).GetError(), true))), ...);
        return failure;
    }

    template <std::size_t... I>
    Return Apply(const std::tuple<Result<Args>...>& decoded, std::index_sequence<I...>) const {
        return function(std::get<I>(decoded).Value()...);
    }

    Return (*function)(Args...);
};

class HleCallRegistry final {
public:
    // Refuses an unsupported signature here, so a guest can never reach a
    // function this layer cannot marshal (acceptance H04).
    template <typename Function>
    [[nodiscard]] Result<std::uint64_t> Register(Function function, std::string name) {
        auto adapter = std::make_shared<TypedHleCallAdapter<Function>>(function);
        if (!adapter->SignatureSupported()) {
            return MakeError(ErrorCategory::Unsupported, "HleCallRegistry::Register",
                             name + ": " + adapter->SignatureDescription());
        }
        std::unique_lock guard{registry_mutex};
        const std::uint64_t operation = next_operation++;
        adapter->Assign(operation, std::move(name));
        adapters.push_back(std::move(adapter));
        return operation;
    }

    [[nodiscard]] std::shared_ptr<HleCallAdapter> Find(std::uint64_t operation) const {
        std::shared_lock guard{registry_mutex};
        for (const auto& adapter : adapters) {
            if (adapter->Operation() == operation) {
                return adapter;
            }
        }
        return nullptr;
    }

    // Dispatch entry. An unregistered operation is an error, never an attempt
    // to treat rax as a host syscall number (§8).
    [[nodiscard]] Status Dispatch(HleCallFrame& frame) const {
        auto adapter = Find(frame.operation);
        if (adapter == nullptr) {
            return MakeError(ErrorCategory::InvalidHandle, "HleCallRegistry::Dispatch",
                             "operation " + std::to_string(frame.operation) +
                                 " is not registered");
        }
        return adapter->Invoke(frame);
    }

    [[nodiscard]] std::size_t Size() const {
        std::shared_lock guard{registry_mutex};
        return adapters.size();
    }

private:
    mutable std::shared_mutex registry_mutex;
    std::uint64_t next_operation{1};
    std::vector<std::shared_ptr<HleCallAdapter>> adapters;
};

} // namespace Core::GuestCpu::Hle
