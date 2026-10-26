#pragma once

#include "hook_backend.hpp"
#include "result.hpp"
#include "runtime_state.hpp"
#include "thread_attachment.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

struct Il2CppClass;
struct Il2CppDomain;
struct Il2CppFieldInfo;
struct Il2CppMethodInfo;
struct Il2CppObject;
struct Il2CppString;

namespace il2cpp_native_bridge {

class ClassQuery;
class Runtime;
struct ClassHandle;
struct FieldHandle;
struct MethodHandle;
struct MethodMetadata;

/// 宿主注入的 IL2CPP 导出符号查找函数。
using RuntimeSymbolLookup = void* (*)(void* module, const char* symbol_name);
/// 判断模块是否已可被 RuntimeSymbolLookup 扫描。
using RuntimeSymbolLookupReady = bool (*)(void* module);

namespace detail {
struct Api;
struct HookState;
struct RuntimeContext;

/// 返回 Hook 的原函数跳板；state 由 HookHandle 持有的不透明指针，移除后返回空指针。
void* hook_original(const void* state) noexcept;

/// 以注入的导出表构造 Runtime，仅供测试使用。
Runtime make_testing_runtime(Api api, Il2CppDomain* domain);
} /// namespace detail

struct ClassHandle {
    std::shared_ptr<detail::RuntimeContext> context;
    Il2CppClass* native{};
    std::uint64_t generation{};
    std::string full_name;
    std::string assembly_name;
};

struct MethodHandle {
    ClassHandle declaring_class;
    const Il2CppMethodInfo* native{};
    std::string name;
    std::string return_type;
    std::vector<std::string> parameter_types;
    bool is_static{};
};

struct FieldHandle {
    ClassHandle declaring_class;
    const Il2CppFieldInfo* native{};
    std::string name;
    std::string value_type;
    bool is_static{};
};

struct MethodMetadata {
    std::string name;
    std::string return_type;
    std::vector<std::string> parameter_types;
    bool is_static{};
};

struct RuntimeWaitOptions {
    std::chrono::milliseconds poll_interval{100};
    std::chrono::milliseconds timeout{10000};
    std::function<void(const RuntimeSnapshot&, std::chrono::milliseconds)> on_timeout;
};

template <typename T>
struct ManagedTypeName;

template <> struct ManagedTypeName<void> { static constexpr std::string_view value = "System.Void"; };
template <> struct ManagedTypeName<bool> { static constexpr std::string_view value = "System.Boolean"; };
template <> struct ManagedTypeName<std::int8_t> { static constexpr std::string_view value = "System.SByte"; };
template <> struct ManagedTypeName<std::uint8_t> { static constexpr std::string_view value = "System.Byte"; };
template <> struct ManagedTypeName<std::int16_t> { static constexpr std::string_view value = "System.Int16"; };
template <> struct ManagedTypeName<std::uint16_t> { static constexpr std::string_view value = "System.UInt16"; };
template <> struct ManagedTypeName<std::int32_t> { static constexpr std::string_view value = "System.Int32"; };
template <> struct ManagedTypeName<std::uint32_t> { static constexpr std::string_view value = "System.UInt32"; };
template <> struct ManagedTypeName<std::int64_t> { static constexpr std::string_view value = "System.Int64"; };
template <> struct ManagedTypeName<std::uint64_t> { static constexpr std::string_view value = "System.UInt64"; };
template <> struct ManagedTypeName<float> { static constexpr std::string_view value = "System.Single"; };
template <> struct ManagedTypeName<double> { static constexpr std::string_view value = "System.Double"; };
template <> struct ManagedTypeName<Il2CppObject*> { static constexpr std::string_view value = "System.Object"; };
template <> struct ManagedTypeName<Il2CppString*> { static constexpr std::string_view value = "System.String"; };

template <typename T>
inline constexpr std::string_view managed_type_name = ManagedTypeName<std::remove_cvref_t<T>>::value;

class Runtime {
public:
    static Runtime current();
    static Result<void> set_symbol_lookup(void* module,
                                          RuntimeSymbolLookup lookup,
                                          RuntimeSymbolLookupReady is_ready = nullptr);
    static Result<void> set_export_override(std::string_view symbol_name, void* address);

    [[nodiscard]] RuntimeSnapshot snapshot() const;
    Result<void> refresh() const;

    /**
     * 在后台线程等待 IL2CPP runtime 就绪，再附加该线程并执行 action。
     *
     * waiting_for_module/initializing 状态会持续轮询；超过 options.timeout 时最多调用一次
     * options.on_timeout，然后继续等待。unsupported runtime、线程附加错误或 action 错误
     * 会传给 completion，不会重试 action。action、超时和完成回调都在后台线程执行，其捕获对象
     * 必须在任务结束前保持有效。启动成功后 completion 恰好调用一次；启动失败时直接返回错误，
     * 不调用 completion。该方法返回只表示后台任务已成功启动。
     */
    Result<void> perform_when_ready(const std::function<Result<void>()>& action,
                                    std::function<void(Result<void>)> completion,
                                    RuntimeWaitOptions options = {}) const;

    /**
     * 在当前线程附加到 IL2CPP 的作用域内同步执行回调。
     *
     * runtime 未就绪或线程附加失败时不会执行回调；仅当本次调用完成附加时，回调结束后才分离线程。
     *
     * @param action 要在附加作用域内执行的操作，不能为空。
     * @return 回调结果；action 为空、runtime 未就绪、线程 API 缺失或附加失败时返回对应错误，
     *         回调自身的错误原样传回。
     * @throws action 抛出的异常会传给调用方，线程附加作用域会在栈展开时释放。
     */
    Result<void> with_attached_thread(const std::function<Result<void>()>& action) const;
    Result<std::vector<ClassQuery>> classes(std::string_view name_filter = {}) const;
    ClassQuery klass(std::string_view fully_qualified_name) const;
    ClassQuery klass_of(Il2CppObject* instance) const;
    ClassQuery generic_class(std::string_view generic_type_name,
                             std::span<const std::string_view> type_arguments,
                             std::optional<std::string_view> assembly_name = std::nullopt) const;
    Result<ClassHandle> class_by_name(std::string_view fully_qualified_name,
                                      std::optional<std::string> assembly_name = std::nullopt) const;

private:
    explicit Runtime(std::shared_ptr<detail::RuntimeContext> context) : context_(std::move(context)) {}
    std::shared_ptr<detail::RuntimeContext> context_;

    friend class ThreadAttachment;
    friend class ClassQuery;
    friend Runtime detail::make_testing_runtime(detail::Api api, Il2CppDomain* domain);
};

template <typename Return, typename... Args>
class MethodQuery;

template <typename T>
class FieldQuery;

class ClassQuery {
public:
    ClassQuery in_assembly(std::string_view assembly_name) const;
    [[nodiscard]] bool found() const noexcept { return result_.has_value(); }
    [[nodiscard]] const Error& error() const { return result_.error(); }
    [[nodiscard]] std::string_view full_name() const noexcept {
        return result_ ? std::string_view(result_.value().full_name) : std::string_view(full_name_);
    }
    [[nodiscard]] std::string_view assembly_name() const noexcept {
        if (result_) return result_.value().assembly_name;
        return assembly_name_ ? std::string_view(*assembly_name_) : std::string_view{};
    }

    template <typename Return, typename... Args>
    MethodQuery<Return, Args...> method(std::string_view name) const;

    template <typename Return, typename... Args>
    MethodQuery<Return, Args...> method(std::string_view name, std::size_t managed_parameter_count) const;

    template <typename T>
    FieldQuery<T> field(std::string_view name) const;

    FieldQuery<Il2CppObject*> object_field(std::string_view name) const;
    FieldQuery<Il2CppObject*> boxed_field(std::string_view name) const;
    Result<std::vector<MethodMetadata>> methods() const;
    Result<std::vector<FieldHandle>> fields() const;
    Result<Il2CppObject*> new_object() const;

private:
    ClassQuery(Runtime runtime, std::string full_name, std::optional<std::string> assembly_name);
    ClassQuery(Runtime runtime, Result<ClassHandle> result);

    Result<MethodHandle> find_method(std::string_view name,
                                     std::string_view return_type,
                                     std::vector<std::string> parameter_types) const;
    Result<MethodHandle> find_method_by_arity(std::string_view name, std::size_t parameter_count) const;
    Result<FieldHandle> find_field(std::string_view name, std::string_view value_type) const;

    Runtime runtime_;
    std::string full_name_;
    std::optional<std::string> assembly_name_;
    Result<ClassHandle> result_;

    friend class Runtime;
};

namespace detail {

/// FieldQuery 的非模板基类：持有已解析字段并提供按字节读写的实现入口。
class FieldQueryBase {
public:
    [[nodiscard]] bool found() const noexcept { return field_.has_value(); }
    [[nodiscard]] const Error& error() const { return field_.error(); }
    [[nodiscard]] std::string_view value_type_name() const noexcept {
        return field_ ? std::string_view(field_.value().value_type) : std::string_view{};
    }

protected:
    explicit FieldQueryBase(Result<FieldHandle> field) : field_(std::move(field)) {}
    Result<void> read_value(Il2CppObject* instance, void* destination, std::size_t size) const;
    Result<void> write_value(Il2CppObject* instance, const void* source, std::size_t size) const;
    Result<void> write_boxed_value(Il2CppObject* boxed_value, Il2CppObject* instance) const;

    Result<FieldHandle> field_;
};

} /// namespace detail

template <typename T>
class FieldQuery : public detail::FieldQueryBase {
public:
    Result<T> get(Il2CppObject* instance = nullptr) const {
        T value{};
        auto read = read_value(instance, &value, sizeof(T));
        if (!read) return Result<T>::failure(read.error());
        return Result<T>::success(value);
    }

    Result<void> set(const T& value, Il2CppObject* instance = nullptr) const {
        T copy = value;
        return write_value(instance, &copy, sizeof(T));
    }

    Result<void> set_boxed(Il2CppObject* boxed_value, Il2CppObject* instance = nullptr) const {
        return write_boxed_value(boxed_value, instance);
    }

private:
    explicit FieldQuery(Result<FieldHandle> field) : detail::FieldQueryBase(std::move(field)) {}

    friend class ClassQuery;
};

/**
 * 持有一个已安装的 Hook；此类型不可复制，可移动。
 *
 * 析构时会尝试移除 Hook，但不会报告析构期间的移除错误。需要检查错误时，应显式调用 remove()。
 */
template <typename Return, typename... Args>
class HookHandle {
public:
    using Function = Return (*)(Args...);

    HookHandle() = default;
    explicit HookHandle(std::shared_ptr<detail::HookState> state) : state_(std::move(state)) {}
    HookHandle(const HookHandle&) = delete;
    HookHandle& operator=(const HookHandle&) = delete;
    HookHandle(HookHandle&&) noexcept = default;
    HookHandle& operator=(HookHandle&&) noexcept = default;
    ~HookHandle() { if (state_) (void) detail::remove_hook(state_); }

    /** @return 原函数跳板；Hook 已移除或句柄为空时返回空指针。 */
    [[nodiscard]] Function original() const noexcept;

    /**
     * 显式移除 Hook。
     *
     * @return 移除结果；重复移除会成功返回，backend 移除失败时返回其错误。
     */
    Result<void> remove() { return detail::remove_hook(state_); }

private:
    std::shared_ptr<detail::HookState> state_;
};

namespace detail {

/// MethodQuery 的非模板基类：持有已解析方法并提供调用与安装的实现入口。
class MethodQueryBase {
public:
    [[nodiscard]] bool found() const noexcept { return method_.has_value(); }
    [[nodiscard]] const Error& error() const { return method_.error(); }

protected:
    explicit MethodQueryBase(Result<MethodHandle> method) : method_(std::move(method)) {}
    Result<Il2CppObject*> invoke_value(Il2CppObject* instance, std::span<void*> arguments) const;
    Result<void> copy_value(Il2CppObject* value, void* destination, std::size_t size) const;
    Result<std::shared_ptr<HookState>> install_value(std::shared_ptr<HookBackend> backend,
                                                     void* replacement) const;

    Result<MethodHandle> method_;
};

} /// namespace detail

template <typename Return, typename... Args>
class MethodQuery : public detail::MethodQueryBase {
public:
    Result<Return> invoke(Args... args) const { return invoke_on(nullptr, std::forward<Args>(args)...); }

    Result<Return> invoke_on(Il2CppObject* instance, Args... args) const {
        if (!method_) return Result<Return>::failure(method_.error());
        if (!method_.value().is_static && instance == nullptr) {
            return Result<Return>::failure(make_error(ErrorCode::invalid_instance, "instance method requires a managed object"));
        }
        auto values = std::tuple<std::remove_cvref_t<Args>...>(std::forward<Args>(args)...);
        auto pointers = argument_pointers(values, std::index_sequence_for<Args...>{});
        auto result = invoke_value(instance, std::span<void*>(pointers.data(), pointers.size()));
        if (!result) return Result<Return>::failure(result.error());

        if constexpr (std::is_void_v<Return>) {
            return Result<void>::success();
        } else if constexpr (std::is_pointer_v<Return>) {
            return Result<Return>::success(reinterpret_cast<Return>(result.value()));
        } else {
            Return value{};
            auto copied = copy_value(result.value(), &value, sizeof(Return));
            if (!copied) return Result<Return>::failure(copied.error());
            return Result<Return>::success(value);
        }
    }

    Result<HookHandle<Return, Args...>> hook(std::shared_ptr<HookBackend> backend,
                                             Return (*replacement)(Args...)) const {
        using Handle = HookHandle<Return, Args...>;
        auto installed = install_value(std::move(backend), reinterpret_cast<void*>(replacement));
        if (!installed) return Result<Handle>::failure(installed.error());
        return Result<Handle>::success(Handle(installed.take_value()));
    }

    Result<HookHandle<Return, Il2CppObject*, Args...>> hook_instance(
        std::shared_ptr<HookBackend> backend,
        Return (*replacement)(Il2CppObject*, Args...)) const {
        using Handle = HookHandle<Return, Il2CppObject*, Args...>;
        auto installed = install_value(std::move(backend), reinterpret_cast<void*>(replacement));
        if (!installed) return Result<Handle>::failure(installed.error());
        return Result<Handle>::success(Handle(installed.take_value()));
    }

private:
    explicit MethodQuery(Result<MethodHandle> method) : detail::MethodQueryBase(std::move(method)) {}

    template <typename Tuple, std::size_t... Indices>
    static std::array<void*, sizeof...(Indices)> argument_pointers(Tuple& values, std::index_sequence<Indices...>) {
        return {static_cast<void*>(std::addressof(std::get<Indices>(values)))...};
    }

    friend class ClassQuery;
};

template <typename Return, typename... Args>
typename HookHandle<Return, Args...>::Function HookHandle<Return, Args...>::original() const noexcept {
    if (!state_) return nullptr;
    return reinterpret_cast<Function>(detail::hook_original(state_.get()));
}

template <typename Return, typename... Args>
MethodQuery<Return, Args...> ClassQuery::method(std::string_view name) const {
    static_assert((requires { ManagedTypeName<std::remove_cvref_t<Args>>::value; } && ...), "method argument type has no IL2CPP name mapping");
    static_assert(requires { ManagedTypeName<std::remove_cvref_t<Return>>::value; }, "method result type has no IL2CPP name mapping");
    std::vector<std::string> parameters;
    parameters.reserve(sizeof...(Args));
    (parameters.emplace_back(managed_type_name<Args>), ...);
    return MethodQuery<Return, Args...>(find_method(name, managed_type_name<Return>, std::move(parameters)));
}

template <typename Return, typename... Args>
MethodQuery<Return, Args...> ClassQuery::method(std::string_view name,
                                                std::size_t managed_parameter_count) const {
    return MethodQuery<Return, Args...>(find_method_by_arity(name, managed_parameter_count));
}

template <typename T>
FieldQuery<T> ClassQuery::field(std::string_view name) const {
    static_assert(requires { ManagedTypeName<std::remove_cvref_t<T>>::value; }, "field type has no IL2CPP name mapping");
    return FieldQuery<T>(find_field(name, managed_type_name<T>));
}

} /// namespace il2cpp_native_bridge
