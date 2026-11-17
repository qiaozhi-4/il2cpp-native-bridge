#pragma once

#include <string>
#include <optional>
#include <utility>
#include <variant>

namespace INB {

/// 错误分类；调用方据此判断是等待重试，还是必须改用其他调用方式。
enum class ErrorCode {
    /// IL2CPP 运行时尚未就绪（模块未加载、解析器未就绪或 domain 无程序集）。
    runtime_not_ready,
    /// 缺少必需的 IL2CPP 导出，当前运行时不受支持。
    unsupported_runtime,
    /// 本次操作所需的导出或能力不可用，不影响其他操作。
    missing_api,
    /// 未找到匹配的类型、方法或字段。
    not_found,
    /// 多个候选同时匹配，调用方需进一步缩小范围。
    ambiguous_match,
    /// 托管签名与请求的 C++ 类型不一致。
    type_mismatch,
    /// 实例方法收到空实例，或实例与声明类不兼容。
    invalid_instance,
    /// 托管调用未产生可用结果。
    invocation_failed,
    /// 托管方法抛出了异常。
    managed_exception,
    /// 方法的原生入口地址不可解析或不在可执行映射内。
    entry_point_unavailable,
    /// Hook 后端不可用。
    hook_backend_unavailable,
    /// Hook 安装或移除失败。
    hook_install_failed,
    /// 句柄属于旧的 runtime 代际，不能继续使用。
    stale_handle,
    /// 参数不满足调用前提。
    invalid_argument,
    /// 库内部错误。
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
    /// 未设置阶段，旧调用方与库内部兜底使用。
    none,
    /// 提交 perform 请求阶段。
    submit,
    /// 等待 IL2CPP runtime 就绪阶段。
    readiness,
    /// 进入或校验运行时访问作用域阶段。
    scope,
    /// 类型、方法或字段查找阶段。
    lookup,
    /// 静态签名绑定阶段。
    bind,
    /// 托管方法调用阶段。
    invoke,
    /// 安装或执行 managed 替换阶段。
    replacement,
    /// 恢复 managed 替换阶段。
    restore,
    /// 完成回调阶段。
    completion,
};

/**
 * 一次失败的描述；Error 本身不拥有动态资源，可跨线程按值传递。
 *
 * stage 与 path 只对采用结构化错误的新门面有意义，旧调用方保持默认空值。
 */
struct Error {
    /// 错误分类。
    ErrorCode code{ErrorCode::internal_error};
    /// 面向诊断的描述，不保证跨版本稳定，调用方不应据此做逻辑判断。
    std::string message;
    /// 失败阶段，见 ErrorStage。
    ErrorStage stage{ErrorStage::none};
    /// 已知的域/程序集/类/方法名称路径，只用于定位，不含对象内容或设备信息。
    std::string path;
};

/**
 * 构造不带阶段与路径的错误，供旧门面与内部辅助函数使用。
 * @param code 错误分类。
 * @param message 面向诊断的描述。
 * @return 对应错误对象，stage 为 none、path 为空。
 */
inline Error make_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    return error;
}

/**
 * 构造带阶段与定位路径的结构化错误，供新门面使用。
 * @param code 错误分类。
 * @param stage 失败阶段。
 * @param path 定位用的名称路径。
 * @param message 面向诊断的描述。
 * @return 对应错误对象。
 */
inline Error make_error(ErrorCode code, ErrorStage stage, std::string path, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    error.stage = stage;
    error.path = std::move(path);
    return error;
}

/**
 * 值或错误的二选一容器；同一时刻只持有其中一种。
 *
 * 成功值通过 success() 构造，失败通过 failure() 构造；取用前先用 has_value() 或
 * 布尔转换判断，避免在失败结果上调用 value()、在成功结果上调用 error()。
 *
 * @tparam T 成功值的类型。
 */
template <typename T>
class Result {
public:
    /**
     * 构造成功结果。
     * @param value 成功值，按值保存。
     * @return 持有该值的结果。
     */
    static Result success(T value) { return Result(std::move(value)); }
    /**
     * 构造失败结果。
     * @param error 失败描述。
     * @return 持有该错误的结果。
     */
    static Result failure(Error error) { return Result(std::move(error)); }

    /// 当前是否持有成功值。
    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
    /// 与 has_value() 等价，便于在条件中直接使用。
    explicit operator bool() const noexcept { return has_value(); }

    /**
     * 取成功值的引用；调用方须先确认 has_value() 为真。
     * @return 成功值的引用。
     */
    T& value() { return std::get<T>(storage_); }
    /// value() 的常量重载；调用方须先确认 has_value() 为真。
    const T& value() const { return std::get<T>(storage_); }
    /**
     * 取走成功值，仅对右值结果使用。
     * @return 成功值的右值引用。
     */
    T&& take_value() { return std::get<T>(std::move(storage_)); }

    /**
     * 取失败描述；调用方须先确认 has_value() 为假。
     * @return 失败描述的引用。
     */
    const Error& error() const { return std::get<Error>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

/**
 * Result 的无值特化，用于表达只报告成功或失败的操作。
 *
 * 成功时内部不保存任何值；失败时保存一份 Error。取 error() 前须先确认 has_value() 为假。
 */
template <>
class Result<void> {
public:
    /**
     * 构造成功结果。
     * @return 不持有错误的结果。
     */
    static Result success() { return Result(); }
    /**
     * 构造失败结果。
     * @param error 失败描述。
     * @return 持有该错误的结果。
     */
    static Result failure(Error error) { return Result(std::move(error)); }

    /// 当前是否表示成功（即不持有错误）。
    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    /// 与 has_value() 等价，便于在条件中直接使用。
    explicit operator bool() const noexcept { return has_value(); }
    /**
     * 取失败描述；调用方须先确认 has_value() 为假。
     * @return 失败描述的引用。
     */
    const Error& error() const { return *error_; }

private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

} /// namespace INB
