#pragma once

#include "detail/il2cpp_api.hpp"
#include "hook_backend.hpp"
#include "result.hpp"
#include "runtime_state.hpp"
#include "thread_attachment.hpp"

#include <array>
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

namespace il2cpp_native_bridge {

struct ClassHandle;
struct MethodHandle;
struct FieldHandle;
struct MethodMetadata;

using RuntimeSymbolLookup = detail::SymbolLookup;

namespace detail {
struct RuntimeContext;
struct HookState;
Api api_for(const std::shared_ptr<RuntimeContext>& context);
void* original_function(const std::shared_ptr<HookState>& state) noexcept;

struct InvokeOutcome {
    Il2CppObject* value{};
};

Result<ClassHandle> resolve_class(const std::shared_ptr<RuntimeContext>& context,
                                  std::string_view full_name,
                                  const std::optional<std::string>& assembly_name);
Result<std::vector<ClassHandle>> resolve_classes(const std::shared_ptr<RuntimeContext>& context,
                                                 std::string_view name_filter);
Result<ClassHandle> resolve_generic_class(const std::shared_ptr<RuntimeContext>& context,
                                          std::string_view generic_type_name,
                                          std::span<const std::string_view> type_arguments,
                                          const std::optional<std::string>& assembly_name);
Result<ClassHandle> resolve_object_class(const std::shared_ptr<RuntimeContext>& context, Il2CppObject* instance);
Result<std::vector<MethodMetadata>> resolve_methods(const ClassHandle& klass);
Result<std::vector<FieldHandle>> resolve_fields(const ClassHandle& klass);
Result<Il2CppObject*> allocate_object(const ClassHandle& klass);
Result<MethodHandle> resolve_method(const ClassHandle& klass,
                                    std::string_view name,
                                    std::string_view return_type,
                                    const std::vector<std::string>& parameter_types);
Result<MethodHandle> resolve_method_by_arity(const ClassHandle& klass,
                                             std::string_view name,
                                             std::size_t parameter_count);
Result<FieldHandle> resolve_field(const ClassHandle& klass,
                                  std::string_view name,
                                  std::string_view value_type);
Result<void> read_field_raw(const FieldHandle& field, Il2CppObject* instance, void* destination, std::size_t size);
Result<void> write_field_raw(const FieldHandle& field, Il2CppObject* instance, void* source, std::size_t size);
Result<void> write_boxed_field(const FieldHandle& field, Il2CppObject* boxed_value, Il2CppObject* instance);
Result<InvokeOutcome> invoke_raw(const MethodHandle& method, Il2CppObject* instance, std::span<void*> arguments);
Result<void*> resolve_entry_point(const MethodHandle& method);
Result<std::shared_ptr<HookState>> install_hook(const MethodHandle& method,
                                                std::shared_ptr<HookBackend> backend,
                                                void* replacement);
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

class Runtime;
class ClassQuery;

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
    static Result<void> set_symbol_lookup(void* module, RuntimeSymbolLookup lookup);
    static Result<void> set_export_override(std::string_view symbol_name, void* address);
    static Runtime for_testing(detail::Api api, Il2CppDomain* domain);

    [[nodiscard]] RuntimeSnapshot snapshot() const;
    Result<void> refresh() const;

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
    Runtime runtime_;
    std::string full_name_;
    std::optional<std::string> assembly_name_;
    Result<ClassHandle> result_;

    friend class Runtime;
};

template <typename T>
class FieldQuery {
public:
    [[nodiscard]] bool found() const noexcept { return field_.has_value(); }
    [[nodiscard]] const Error& error() const { return field_.error(); }

    Result<T> get(Il2CppObject* instance = nullptr) const {
        if (!field_) return Result<T>::failure(field_.error());
        T value{};
        auto read = detail::read_field_raw(field_.value(), instance, &value, sizeof(T));
        if (!read) return Result<T>::failure(read.error());
        return Result<T>::success(value);
    }

    Result<void> set(const T& value, Il2CppObject* instance = nullptr) const {
        if (!field_) return Result<void>::failure(field_.error());
        T copy = value;
        return detail::write_field_raw(field_.value(), instance, &copy, sizeof(T));
    }

    [[nodiscard]] std::string_view value_type_name() const noexcept {
        return field_ ? std::string_view(field_.value().value_type) : std::string_view{};
    }

    Result<void> set_boxed(Il2CppObject* boxed_value, Il2CppObject* instance = nullptr) const {
        if (!field_) return Result<void>::failure(field_.error());
        return detail::write_boxed_field(field_.value(), boxed_value, instance);
    }

private:
    explicit FieldQuery(Result<FieldHandle> field) : field_(std::move(field)) {}
    Result<FieldHandle> field_;

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

template <typename Return, typename... Args>
class MethodQuery {
public:
    [[nodiscard]] bool found() const noexcept { return method_.has_value(); }
    [[nodiscard]] const Error& error() const { return method_.error(); }

    Result<Return> invoke(Args... args) const { return invoke_on(nullptr, std::forward<Args>(args)...); }

    Result<Return> invoke_on(Il2CppObject* instance, Args... args) const {
        if (!method_) return Result<Return>::failure(method_.error());
        if (!method_.value().is_static && instance == nullptr) {
            return Result<Return>::failure(make_error(ErrorCode::invalid_instance, "instance method requires a managed object"));
        }
        auto values = std::tuple<std::remove_cvref_t<Args>...>(std::forward<Args>(args)...);
        auto pointers = argument_pointers(values, std::index_sequence_for<Args...>{});
        auto result = detail::invoke_raw(method_.value(), instance,
                                         std::span<void*>(pointers.data(), pointers.size()));
        if (!result) return Result<Return>::failure(result.error());

        if constexpr (std::is_void_v<Return>) {
            return Result<void>::success();
        } else if constexpr (std::is_pointer_v<Return>) {
            return Result<Return>::success(reinterpret_cast<Return>(result.value().value));
        } else {
            if (result.value().value == nullptr) {
                return Result<Return>::failure(make_error(ErrorCode::invocation_failed, "value-type invocation returned null"));
            }
            const auto api = detail::api_for(method_.value().declaring_class.context);
            if (api.object_unbox == nullptr) {
                return Result<Return>::failure(make_error(ErrorCode::missing_api, "il2cpp_object_unbox is unavailable"));
            }
            void* unboxed = api.object_unbox(result.value().value);
            if (unboxed == nullptr) {
                return Result<Return>::failure(make_error(ErrorCode::invocation_failed, "could not unbox method result"));
            }
            Return value{};
            std::memcpy(&value, unboxed, sizeof(Return));
            return Result<Return>::success(value);
        }
    }

    Result<HookHandle<Return, Args...>> hook(std::shared_ptr<HookBackend> backend,
                                             Return (*replacement)(Args...)) const {
        if (!method_) return Result<HookHandle<Return, Args...>>::failure(method_.error());
        auto installed = detail::install_hook(method_.value(), std::move(backend), reinterpret_cast<void*>(replacement));
        if (!installed) return Result<HookHandle<Return, Args...>>::failure(installed.error());
        return Result<HookHandle<Return, Args...>>::success(HookHandle<Return, Args...>(installed.take_value()));
    }

    Result<HookHandle<Return, Il2CppObject*, Args...>> hook_instance(
        std::shared_ptr<HookBackend> backend,
        Return (*replacement)(Il2CppObject*, Args...)) const {
        using InstanceHook = HookHandle<Return, Il2CppObject*, Args...>;
        if (!method_) return Result<InstanceHook>::failure(method_.error());
        auto installed = detail::install_hook(method_.value(), std::move(backend), reinterpret_cast<void*>(replacement));
        if (!installed) return Result<InstanceHook>::failure(installed.error());
        return Result<InstanceHook>::success(InstanceHook(installed.take_value()));
    }

private:
    explicit MethodQuery(Result<MethodHandle> method) : method_(std::move(method)) {}

    template <typename Tuple, std::size_t... Indices>
    static std::array<void*, sizeof...(Indices)> argument_pointers(Tuple& values, std::index_sequence<Indices...>) {
        return {static_cast<void*>(std::addressof(std::get<Indices>(values)))...};
    }

    Result<MethodHandle> method_;

    friend class ClassQuery;
};

template <typename Return, typename... Args>
typename HookHandle<Return, Args...>::Function HookHandle<Return, Args...>::original() const noexcept {
    if (!state_) return nullptr;
    return reinterpret_cast<Function>(detail::original_function(state_));
}

template <typename Return, typename... Args>
MethodQuery<Return, Args...> ClassQuery::method(std::string_view name) const {
    static_assert((requires { ManagedTypeName<std::remove_cvref_t<Args>>::value; } && ...), "method argument type has no IL2CPP name mapping");
    static_assert(requires { ManagedTypeName<std::remove_cvref_t<Return>>::value; }, "method result type has no IL2CPP name mapping");
    if (!result_) return MethodQuery<Return, Args...>(Result<MethodHandle>::failure(result_.error()));
    const std::vector<std::string> parameters{std::string(managed_type_name<Args>)...};
    return MethodQuery<Return, Args...>(detail::resolve_method(result_.value(), name,
        managed_type_name<Return>, parameters));
}

template <typename Return, typename... Args>
MethodQuery<Return, Args...> ClassQuery::method(std::string_view name,
                                                std::size_t managed_parameter_count) const {
    if (!result_) return MethodQuery<Return, Args...>(Result<MethodHandle>::failure(result_.error()));
    return MethodQuery<Return, Args...>(detail::resolve_method_by_arity(
        result_.value(), name, managed_parameter_count));
}

template <typename T>
FieldQuery<T> ClassQuery::field(std::string_view name) const {
    static_assert(requires { ManagedTypeName<std::remove_cvref_t<T>>::value; }, "field type has no IL2CPP name mapping");
    if (!result_) return FieldQuery<T>(Result<FieldHandle>::failure(result_.error()));
    return FieldQuery<T>(detail::resolve_field(result_.value(), name, managed_type_name<T>));
}

} /// namespace il2cpp_native_bridge
