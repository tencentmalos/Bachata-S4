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

// A pointer to a function is a guest CODE address (a callback the HLE stores and
// later enters via InvokeGuest), not a data buffer the host dereferences. It is
// refused until a guest-code handle and an audited callback descriptor exist.
// A native C++ function pointer is callable, not an opaque guest address.
template <typename T>
inline constexpr bool IsGuestFunctionPointer =
    std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

// Written as a lambda so `sizeof(T)` is never instantiated for void, which is
// an incomplete type.
template <typename T>
inline constexpr bool IsIntegerArgument = [] {
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        return sizeof(T) <= sizeof(std::uint64_t);
    } else {
        return false;
    }
}();

template <typename T>
inline constexpr bool IsVectorArgument = std::is_same_v<T, float> || std::is_same_v<T, double>;

// Detects a GuestBoundedBuffer<U>.
template <typename T>
inline constexpr bool IsBoundedBuffer = false;
template <typename U>
inline constexpr bool IsBoundedBuffer<GuestBoundedBuffer<U>> = true;

template <typename T>
inline constexpr bool IsPinnablePointer = [] {
    if constexpr (!IsGuestPointer<T> || IsGuestFunctionPointer<T>)
        return false;
    else {
        using P = std::remove_pointer_t<T>;
        return std::is_void_v<P> || requires { sizeof(P); };
    }
}();

// V0 accepts scalars, validated guest pointers and bounded buffers. Aggregates
// and varargs are refused at registration (§8).
template <typename T>
inline constexpr bool IsSupportedArgument =
    IsIntegerArgument<T> || IsVectorArgument<T> || IsPinnablePointer<T> || IsBoundedBuffer<T>;

template <typename T>
inline constexpr bool IsSupportedReturn =
    std::is_void_v<T> || IsIntegerArgument<T> || IsVectorArgument<T> ||
    (IsGuestPointer<T> && !IsGuestFunctionPointer<T>);

// A guest pointer argument is only handed to native code after its range and
// permissions have been checked in the guest address space.
template <typename T>
Result<T> DecodePointer(CallCursor& cursor, HleCallFrame& frame) {
    auto raw = cursor.NextInteger();
    if (!raw) {
        return raw.GetError();
    }
    // Never turn a guest code address into a callable ARM64 function pointer.
    if constexpr (IsGuestFunctionPointer<T>) {
        return MakeError(ErrorCategory::Unsupported, "Hle::DecodePointer",
                         "native function-pointer callbacks require a guest-call descriptor");
    } else {
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
        // remap/unmap on another owner must not retire the backing while native code holds the
        // pointer. The pin is owned by the frame and released on return (success or failure).
        auto pin = frame.space->AcquirePinnedSpan(range.Value(), writable);
        if (!pin) {
            auto error = pin.GetError();
            error.operation = "Hle::DecodePointer";
            return error;
        }
        frame.pins.push_back(std::move(pin.Value()));
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(raw.Value()));
    }
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

protected:
    explicit HleCallAdapter(std::uint64_t op = 0, std::string label = {})
        : operation(op), name(std::move(label)) {}

private:
    const std::uint64_t operation;
    const std::string name;
};

// The primary template also makes varargs/noexcept/unsupported function forms
// representable without instantiating an incomplete marshaller.
template <typename Function>
class TypedHleCallAdapter final : public HleCallAdapter {
public:
    explicit TypedHleCallAdapter(Function) {}
    bool SignatureSupported() const noexcept override {
        return false;
    }
    std::string SignatureDescription() const override {
        return "unsupported function form";
    }
    Status Invoke(HleCallFrame&) const override {
        return MakeError(ErrorCategory::Unsupported, "HleCallAdapter::Invoke",
                         "unsupported function form");
    }
};

template <typename Return, typename... Args>
class TypedHleCallAdapter<Return (*)(Args...)> final : public HleCallAdapter {
public:
    explicit TypedHleCallAdapter(Return (*function_)(Args...)) : function{function_} {}

    [[nodiscard]] bool SignatureSupported() const noexcept override {
        return kSupported && function != nullptr;
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
            if (!function)
                return MakeError(ErrorCategory::InvalidArgument, "HleCallAdapter::Invoke",
                                 "null function");
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

class UnsupportedHleCallAdapter final : public HleCallAdapter {
public:
    explicit UnsupportedHleCallAdapter(std::string description)
        : description_{std::move(description)} {}

    [[nodiscard]] bool SignatureSupported() const noexcept override {
        return false;
    }
    [[nodiscard]] std::string SignatureDescription() const override {
        return description_;
    }
    Status Invoke(HleCallFrame&) const override {
        return MakeError(ErrorCategory::Unsupported, "UnsupportedHleCallAdapter::Invoke",
                         "HLE function has a signature this backend cannot marshal: " +
                             description_);
    }

private:
    std::string description_;
};

// Automatic LIB_FUNCTION registration may only infer scalar ABI shape. A raw
// pointer carries no length, direction, retention or guest-callback policy.
// Keep those symbols inspectable but uncallable until an explicit adapter is supplied.
template <typename Function>
inline constexpr bool AutoHleSignature = false;
template <typename R, typename... A>
inline constexpr bool AutoHleSignature<R (*)(A...)> =
    (std::is_void_v<R> || detail::IsIntegerArgument<R> || detail::IsVectorArgument<R>) &&
    ((detail::IsIntegerArgument<A> || detail::IsVectorArgument<A>) && ...);

template <typename Function>
[[nodiscard]] inline std::shared_ptr<HleCallAdapter> MakeHleAdapter(Function function,
                                                                    std::string name) {
    if constexpr (AutoHleSignature<Function>) {
        auto typed = std::make_shared<TypedHleCallAdapter<Function>>(function);
        if (typed->SignatureSupported())
            return typed;
    }
    return std::make_shared<UnsupportedHleCallAdapter>(
        std::move(name) +
        ": explicit pointer/callback/aggregate descriptor required or null function");
}

// Registration metadata belongs to one registry, never the shared descriptor.
class RegisteredHleCallAdapter final : public HleCallAdapter {
public:
    RegisteredHleCallAdapter(std::uint64_t op, std::string name,
                             std::shared_ptr<HleCallAdapter> target)
        : HleCallAdapter(op, std::move(name)), target_(std::move(target)) {}
    Status Invoke(HleCallFrame& f) const override {
        return target_->Invoke(f);
    }
    bool SignatureSupported() const noexcept override {
        return target_->SignatureSupported();
    }
    std::string SignatureDescription() const override {
        return target_->SignatureDescription();
    }

private:
    std::shared_ptr<HleCallAdapter> target_;
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
        return Adopt(std::move(adapter), std::move(name));
    }

    // Unsupported descriptors remain in SymbolRecord for diagnostics. They never
    // acquire an operation/veneer until their actual marshalling policy is supplied.
    [[nodiscard]] Result<std::uint64_t> Adopt(std::shared_ptr<HleCallAdapter> adapter,
                                              std::string name) {
        if (!adapter)
            return MakeError(ErrorCategory::InvalidArgument, "HleCallRegistry::Adopt",
                             "null adapter");
        if (!adapter->SignatureSupported())
            return MakeError(ErrorCategory::Unsupported, "HleCallRegistry::Adopt",
                             name + ": " + adapter->SignatureDescription());
        std::unique_lock guard{registry_mutex};
        const std::uint64_t operation = adapters.size() + 1;
        auto binding = std::make_shared<RegisteredHleCallAdapter>(operation, std::move(name),
                                                                  std::move(adapter));
        adapters.push_back(std::move(binding));
        return operation;
    }

    // O(1): operations are dense and 1-based, so op N is adapters[N-1].
    [[nodiscard]] std::shared_ptr<HleCallAdapter> Find(std::uint64_t operation) const {
        std::shared_lock guard{registry_mutex};
        if (operation == 0 || operation > adapters.size()) {
            return nullptr;
        }
        return adapters[operation - 1];
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
    std::vector<std::shared_ptr<HleCallAdapter>> adapters;
};

} // namespace Core::GuestCpu::Hle
