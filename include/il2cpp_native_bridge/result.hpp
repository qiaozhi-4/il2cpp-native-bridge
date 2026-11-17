#pragma once

#include <string>
#include <optional>
#include <utility>
#include <variant>

namespace INB {

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
    /// 新门面：在 perform 或 replacement 作用域之外执行了运行时操作。
    scope_violation,
    /// 新门面：有界资源（请求队列、managed 替换名额）已耗尽。
    resource_exhausted,
    /// 新门面：恢复已发出但底层未确认完成，调用方可在之后重试。
    restore_pending,
    /// 新门面：目标地址已由其他所有者（旧 Hook 或外部调用方）管理。
    owner_conflict,
};

/// 失败发生的阶段；旧调用方不设置该字段，保持 none。
enum class ErrorStage {
    none,
    submit,
    readiness,
    scope,
    lookup,
    bind,
    invoke,
    replacement,
    restore,
    completion,
};

struct Error {
    ErrorCode code{ErrorCode::internal_error};
    std::string message;
    ErrorStage stage{ErrorStage::none};
    /// 已知的域/程序集/类/方法名称路径，只用于定位，不含对象内容或设备信息。
    std::string path;
};

inline Error make_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    return error;
}

inline Error make_error(ErrorCode code, ErrorStage stage, std::string path, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    error.stage = stage;
    error.path = std::move(path);
    return error;
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

} /// namespace INB
