#pragma once

#include <string>
#include <optional>
#include <utility>
#include <variant>

namespace il2cpp_native_bridge {

enum class ErrorCode {
    runtime_not_ready,
    unsupported_runtime,
    missing_api,
    not_found,
    ambiguous_match,
    type_mismatch,
    invalid_instance,
    invocation_failed,
    managed_exception,
    entry_point_unavailable,
    hook_backend_unavailable,
    hook_install_failed,
    stale_handle,
    invalid_argument,
    internal_error,
};

struct Error {
    ErrorCode code{ErrorCode::internal_error};
    std::string message;
};

inline Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() { return std::get<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }
    T&& take_value() { return std::get<T>(std::move(storage_)); }

    const Error& error() const { return std::get<Error>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    const Error& error() const { return *error_; }

private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

} /// namespace il2cpp_native_bridge
