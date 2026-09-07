// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A Result<T> that cannot be silently ignored, and cannot be read without
// first checking it. This exists because API contract §3 forbids collapsing a
// failure into a bool.

#pragma once

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "core/guest_cpu/api/status.h"

namespace Core::GuestCpu {

template <typename T>
class [[nodiscard]] Result final {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");

public:
    Result(T value) : storage(std::move(value)) {}
    Result(Error error) : storage(std::move(error)) {
        // A "successful error" is always a construction bug.
        assert(std::get<Error>(storage).IsError());
    }

    [[nodiscard]] bool HasValue() const noexcept {
        return std::holds_alternative<T>(storage);
    }
    explicit operator bool() const noexcept {
        return HasValue();
    }

    [[nodiscard]] T& Value() & {
        assert(HasValue());
        return std::get<T>(storage);
    }
    [[nodiscard]] const T& Value() const& {
        assert(HasValue());
        return std::get<T>(storage);
    }
    [[nodiscard]] T&& Value() && {
        assert(HasValue());
        return std::get<T>(std::move(storage));
    }

    [[nodiscard]] const Error& GetError() const {
        assert(!HasValue());
        return std::get<Error>(storage);
    }
    [[nodiscard]] ErrorCategory Category() const noexcept {
        return HasValue() ? ErrorCategory::None : std::get<Error>(storage).category;
    }

    template <typename U>
    [[nodiscard]] T ValueOr(U&& fallback) const& {
        return HasValue() ? std::get<T>(storage) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> storage;
};

// Void specialisation: still forces the caller to look at the error.
template <>
class [[nodiscard]] Result<void> final {
public:
    Result() = default;
    Result(Error error) : failure(std::move(error)) {
        assert(failure->IsError());
    }

    [[nodiscard]] bool HasValue() const noexcept {
        return !failure.has_value();
    }
    explicit operator bool() const noexcept {
        return HasValue();
    }

    [[nodiscard]] const Error& GetError() const {
        assert(failure.has_value());
        return *failure;
    }
    [[nodiscard]] ErrorCategory Category() const noexcept {
        return failure ? failure->category : ErrorCategory::None;
    }

private:
    std::optional<Error> failure;
};

using Status = Result<void>;

[[nodiscard]] inline Status Ok() {
    return Status{};
}

} // namespace Core::GuestCpu
