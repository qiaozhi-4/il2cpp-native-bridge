#pragma once

#include "bridge.hpp"
#include "result.hpp"
#include "runtime_state.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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

struct Il2CppAssembly;
struct Il2CppClass;
struct Il2CppDomain;
struct Il2CppImage;
struct Il2CppMethodInfo;
struct Il2CppObject;
struct Il2CppType;

namespace INB {

namespace detail {
struct RuntimeContext;
}

class Assembly;
class Class;
class Domain;
class Il2Cpp;
class Image;
class Method;

/// 托管方法替换的状态投影。
enum class ReplacementState {
    /// 当前没有安装替换。
    not_installed,
    /// 替换正在安装，尚未发布为可用。
    installing,
    /// 替换已生效，调用会进入用户回调。
    active,
    /// 正在撤销替换。
    reverting,
    /// 撤销已发出，仍有调用停留在旧入口，需等待排空后再试。
    draining,
};

template <typename Return, typename... Args>
class ReplacementCall;

template <typename Return, typename... Args>
class BoundMethod;

/**
 * 新门面在遇到第一个失败环节时抛出的异常。
 *
 * 异常只在 perform 或替换回调的真实边界被转换为 Result，内部各层不再重复返回错误码。
 */
class BridgeError : public std::exception {
public:
    explicit BridgeError(Error error)
        : error_(std::move(error)), what_(format(error_)) {}

    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] const char* what() const noexcept override { return what_.c_str(); }

private:
    static std::string format(const Error& error) {
        std::string text = "[il2cpp-native-bridge] ";
        if (!error.path.empty()) {
            text += error.path;
            text += ": ";
        }
        text += error.message;
        return text;
    }

    Error error_;
    std::string what_;
};

/// 一次 perform 提交的等待诊断选项。
struct PerformOptions {
    /// 从提交开始计算，超过该时长仍未被调度时最多触发一次 on_wait；0 表示关闭阈值通知。
    std::chrono::milliseconds diagnostic_after{10000};
    /// 等待诊断回调，在协调线程执行；不是运行时访问作用域。
    std::function<void(const RuntimeSnapshot&, std::chrono::milliseconds)> on_wait;
};

/// perform 任务的完成回调，接收任务最终的结果。
using Completion = std::function<void(Result<void>)>;

namespace detail {

/// 判断某个 C++ 类型是否有托管类型名映射；用于把不支持的类型挡在编译期。
template <typename T, typename = void>
struct has_managed_name : std::false_type {};

template <typename T>
struct has_managed_name<T, std::void_t<decltype(ManagedTypeName<std::remove_cvref_t<T>>::value)>>
    : std::true_type {};

/// 记录的固定前缀；由 registry 分配，thunk 与 ReplacementCall 只读这里。
struct ReplacementRecordHeader {
    void* callback{};
    void (*report_failure)(void* record){};
    void* original{};
    bool is_static{};
};

/// 当前线程当前 replacement 的记录；不在回调内时为空。由 provider 实现。
const ReplacementRecordHeader* current_replacement_record();

/**
 * 回调内的运行时访问作用域：让用户在 replacement 回调里也能使用 `Il2Cpp::domain()` 等 API。
 *
 * 由 thunk 在调用用户回调前建立；建立失败（例如运行时已不可用）时静默降级，
 * 不把异常带过 native 栈。
 */
struct ReplacementScope {
    ReplacementScope();
    ~ReplacementScope();
    ReplacementScope(const ReplacementScope&) = delete;
    ReplacementScope& operator=(const ReplacementScope&) = delete;

private:
    void* scope_{};
};

template <typename Return, typename... Args>
struct ReplacementEntry;

/// 类型擦除后的提交入口；模板门面只负责 action 适配与参数校验。
Result<void> submit_perform_request(std::function<Result<void>()> action,
                                    Completion completion,
                                    PerformOptions options);

} /// namespace detail

/**
 * 统一执行入口。所有 action 都在协调线程异步、FIFO、串行执行。
 *
 * 返回值只表示提交是否被接受，不代表 action 已执行或 Hook 已安装。
 */
class Il2Cpp {
public:
    /**
     * 提交动作到协调线程异步执行，动作自动处于运行时访问作用域内，可直接调用 domain() 等门面 API。
     *
     * 动作只能返回 void 或 Result<void>：void 视为成功，Result<void> 原样作为结果。
     * 动作抛出的 BridgeError 或标准异常在工作线程边界转换为失败的 Result<void>，不会传播到提交方。
     * 结果只通过 completion 送达；completion 在协调线程执行，但已离开访问作用域。
     *
     * @tparam Action 可调用对象，调用结果必须是 void 或 Result<void>，否则编译期拒绝。
     * @param action 待执行的动作，按转发捕获为类型擦除的函数。
     * @param completion 完成后至多调用一次的结果回调；回调抛出的异常仅被报告，不改变结果或重跑动作。
     * @param options 等待诊断选项，diagnostic_after 为负会在提交期被拒绝。
     * @return 只表示提交是否被接受；队列已满、执行器停机或提交参数非法时返回失败，
     *         不代表动作已执行或已产生结果。
     */
    template <typename Action>
    static Result<void> perform(Action&& action,
                                Completion completion = {},
                                PerformOptions options = {}) {
        using Outcome = std::invoke_result_t<Action&>;
        static_assert(std::is_void_v<Outcome> || std::is_same_v<std::remove_cvref_t<Outcome>, Result<void>>,
                      "Il2Cpp::perform action must return void or Result<void>");

        /// 统一擦除为 Result<void> 形态，让协调线程只处理一种结果；void 动作补一个成功结果。
        std::function<Result<void>()> erased;
        if constexpr (std::is_void_v<Outcome>) {
            erased = [callable = std::forward<Action>(action)]() mutable -> Result<void> {
                callable();
                return Result<void>::success();
            };
        } else {
            erased = std::forward<Action>(action);
        }
        return detail::submit_perform_request(std::move(erased), std::move(completion), std::move(options));
    }

    /// 进入当前 perform 作用域的域对象；不在作用域内或运行时未就绪时抛 BridgeError。
    static Domain domain();
};

/**
 * 已解析类型的不可变描述：保留原生身份与规范托管名。
 *
 * 类别（值/引用）由绑定期按导出能力判定，不在此缓存，避免把不可用能力当成已知事实。
 */
struct TypeDescriptor {
    const Il2CppType* native{};
    std::string name;
};

/**
 * 已解析方法的不可变描述。
 *
 * 不拥有 runtime context，可在作用域外保存和读取名称/签名；native 指针的有效期由 runtime 卸载决定。
 */
struct MethodDescriptor {
    const Il2CppMethodInfo* native{};
    Il2CppClass* declaring_class{};
    std::string declaring_class_name;
    std::string name;
    TypeDescriptor return_type;
    std::vector<TypeDescriptor> parameter_types;
    bool is_static{};
};

namespace detail {

/// 一次静态签名绑定后的结果。
struct BoundSignature {
    std::shared_ptr<RuntimeContext> context;
    std::uint64_t generation{};
    MethodDescriptor descriptor;
    std::string path;
    /// 每个实参是否为引用载体；invoke 只对这些槽做赋值兼容性核对。
    std::vector<bool> reference_slots;
};

/// 一次安装请求：签名绑定结果 + 用户回调 + 两种槽形态的原生入口。
struct ReplacementRequest {
    BoundSignature signature;
    void* callback{};
    void* static_entry{};
    void* instance_entry{};
};

/// 安装替换；签名不支持、名额耗尽或地址被占用时抛 BridgeError。
void install_replacement(const ReplacementRequest& request);

/// 一个 C++ 实参在托管侧的类别。
struct ArgumentSpec {
    bool reference_carrier{};
    bool string_carrier{};
    std::string_view managed_name;
};

/// 校验唯一候选的静态签名并建立绑定；超出支持范围时抛 BridgeError。
BoundSignature bind_method(const std::shared_ptr<RuntimeContext>& context,
                           std::uint64_t generation,
                           const std::vector<MethodDescriptor>& candidates,
                           std::string path,
                           bool return_is_reference,
                           bool return_is_string,
                           std::string_view return_name,
                           std::span<const ArgumentSpec> arguments,
                           bool allow_byref = false);

/// 核对实例与实参后调用，返回原始托管结果；托管异常抛 managed_exception。
Il2CppObject* invoke_bound(const BoundSignature& binding, Il2CppObject* instance, std::span<void*> arguments);

/// 基础类型返回值从装箱对象复制出来；空结果抛 invocation_failed。
void unbox_result(const BoundSignature& binding, Il2CppObject* boxed, void* destination, std::size_t size);

/**
 * 实参打包：引用类型传对象指针值，基础类型传值地址。
 *
 * 依据 Unity 6000.3.10f1 生成的 `RuntimeInvoker_*`：引用参数是 `(T*)args[i]`，
 * 值参数是 `*(T*)args[i]`（见 build/bridge-perform-unity/g1-invoker-evidence.md）。
 */
template <typename Arg>
void* pack_argument(Arg& value) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<Arg>>) {
        return const_cast<void*>(static_cast<const void*>(value));
    } else {
        return const_cast<void*>(static_cast<const void*>(std::addressof(value)));
    }
}

template <typename Tuple, std::size_t... Indices>
std::array<void*, sizeof...(Indices)> pack_arguments(Tuple& values, std::index_sequence<Indices...>) {
    return {pack_argument(std::get<Indices>(values))...};
}

template <typename Return>
Return convert_result(const BoundSignature& binding, Il2CppObject* raw) {
    if constexpr (std::is_void_v<Return>) {
        (void) binding;
        (void) raw;
        return;
    } else if constexpr (std::is_pointer_v<Return>) {
        (void) binding;
        return reinterpret_cast<Return>(raw);
    } else {
        Return value{};
        unbox_result(binding, raw, &value, sizeof(Return));
        return value;
    }
}

} /// namespace detail

/**
 * 链式查询对象：Domain → Assembly → Image → Class → Method。
 *
 * 只有实际读取运行时元数据的操作要求在 perform 作用域内；已经解析的名称可以带出作用域。
 * 每次操作都核对 context 与 generation，旧代际的句柄明确失败而不是返回过期数据。
 */
class Domain {
public:
    /// 按程序集名选择唯一程序集，容忍一次 `.dll` 后缀；无结果或同名歧义抛 BridgeError。
    [[nodiscard]] Assembly assembly(std::string_view name) const;

private:
    friend class Il2Cpp;
    Domain(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, Il2CppDomain* native)
        : context_(std::move(context)), generation_(generation), native_(native) {}

    std::shared_ptr<detail::RuntimeContext> context_;
    std::uint64_t generation_{};
    Il2CppDomain* native_{};
};

class Assembly {
public:
    /// 该程序集对应的真实 image；不与其他同名程序集合并。
    [[nodiscard]] Image image() const;
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    friend class Domain;
    Assembly(std::shared_ptr<detail::RuntimeContext> context,
             std::uint64_t generation,
             const Il2CppAssembly* native,
             std::string name)
        : context_(std::move(context)), generation_(generation), native_(native), name_(std::move(name)) {}

    std::shared_ptr<detail::RuntimeContext> context_;
    std::uint64_t generation_{};
    const Il2CppAssembly* native_{};
    std::string name_;
};

class Image {
public:
    /// 只在该 image 内查找类，不回退到全域同名类。
    [[nodiscard]] Class klass(std::string_view full_name) const;
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    friend class Assembly;
    Image(std::shared_ptr<detail::RuntimeContext> context,
          std::uint64_t generation,
          const Il2CppImage* native,
          std::string name,
          std::string assembly_name)
        : context_(std::move(context)),
          generation_(generation),
          native_(native),
          name_(std::move(name)),
          assembly_name_(std::move(assembly_name)) {}

    std::shared_ptr<detail::RuntimeContext> context_;
    std::uint64_t generation_{};
    const Il2CppImage* native_{};
    std::string name_;
    std::string assembly_name_;
};

class Class {
public:
    /// 返回候选选择对象；0 个候选立即抛错，多个候选可继续用 overload 缩小范围。
    [[nodiscard]] Method method(std::string_view name) const;
    /// 数量是托管参数个数，不计 this 与隐藏 MethodInfo。
    [[nodiscard]] Method method(std::string_view name, std::size_t managed_parameter_count) const;
    [[nodiscard]] std::string_view full_name() const noexcept { return full_name_; }

private:
    friend class Image;
    Class(std::shared_ptr<detail::RuntimeContext> context,
          std::uint64_t generation,
          Il2CppClass* native,
          std::string full_name,
          std::string image_name,
          std::string assembly_name);

    Method select(std::string_view name, const std::optional<std::size_t>& parameter_count) const;

    std::shared_ptr<detail::RuntimeContext> context_;
    std::uint64_t generation_{};
    Il2CppClass* native_{};
    std::string full_name_;
    std::string image_name_;
    std::string assembly_name_;
};

class Method {
public:
    /// 按参数类型名继续缩小候选；没有候选时抛 not_found。候选不是可执行句柄。
    [[nodiscard]] Method overload(std::span<const std::string_view> parameter_types) const;
    /// 方法短名。
    [[nodiscard]] std::string_view name() const noexcept;

    /// 校验一次静态签名并返回可复用的绑定；签名不支持或候选不唯一时抛 BridgeError。
    template <typename Return, typename... Args>
    [[nodiscard]] BoundMethod<Return, Args...> typed() const;

    /// 静态方法调用；实例方法必须使用 invoke_on。
    template <typename Return, typename... Args>
    Return invoke(Args... args) const;

    /// 在指定实例上调用；实例须与声明类兼容，方法须为非静态。
    template <typename Return, typename... Args>
    Return invoke_on(Il2CppObject* self, Args... args) const;

    /// 安装托管方法替换；由库强拥有，不返回需要保活的句柄。
    template <typename Return, typename... Args>
    void implementation(Return (*callback)(ReplacementCall<Return, Args...>&, Args...)) const;

    /// 恢复当前 managed target；成功后再调用走原实现。
    void revert() const;

    /// 当前 managed 替换的状态。
    [[nodiscard]] ReplacementState replacement_state() const;

private:
    friend class Class;
    Method(std::shared_ptr<detail::RuntimeContext> context,
           std::uint64_t generation,
           Il2CppClass* declaring_class,
           std::string declaring_class_name,
           std::string name,
           std::vector<MethodDescriptor> candidates);

    std::shared_ptr<detail::RuntimeContext> context_;
    std::uint64_t generation_{};
    Il2CppClass* declaring_class_{};
    std::string declaring_class_name_;
    std::string name_;
    std::vector<MethodDescriptor> candidates_;
};

/**
 * 已校验静态签名的可复用绑定。
 *
 * 保存绑定不保护托管实例存活；每次调用仍核对实例兼容性与非空引用实参的赋值兼容性。
 */
template <typename Return, typename... Args>
class BoundMethod {
public:
    Return invoke(Args... args) const { return invoke_on(nullptr, std::forward<Args>(args)...); }

    Return invoke_on(Il2CppObject* self, Args... args) const {
        auto values = std::tuple<std::remove_cvref_t<Args>...>(std::forward<Args>(args)...);
        auto pointers = detail::pack_arguments(values, std::index_sequence_for<Args...>{});
        Il2CppObject* raw = detail::invoke_bound(
                binding_, self, std::span<void*>(pointers.data(), pointers.size()));
        return detail::convert_result<Return>(binding_, raw);
    }

private:
    friend class Method;
    explicit BoundMethod(detail::BoundSignature binding) : binding_(std::move(binding)) {}

    detail::BoundSignature binding_;
};

template <typename Return, typename... Args>
BoundMethod<Return, Args...> Method::typed() const {
    static_assert((detail::has_managed_name<Args>::value && ...),
                  "method argument type has no IL2CPP name mapping");
    static_assert(detail::has_managed_name<Return>::value,
                  "method result type has no IL2CPP name mapping");

    using CleanReturn = std::remove_cvref_t<Return>;
    const std::array<detail::ArgumentSpec, sizeof...(Args)> specifications{
            detail::ArgumentSpec{std::is_pointer_v<std::remove_cvref_t<Args>>,
                                 std::is_same_v<std::remove_cvref_t<Args>, Il2CppString*>,
                                 managed_type_name<Args>}...};
    auto binding = detail::bind_method(context_, generation_, candidates_,
                                       declaring_class_name_ + "::" + name_,
                                       std::is_pointer_v<CleanReturn>,
                                       std::is_same_v<CleanReturn, Il2CppString*>,
                                       managed_type_name<Return>, specifications);
    return BoundMethod<Return, Args...>(std::move(binding));
}

template <typename Return, typename... Args>
Return Method::invoke(Args... args) const {
    return typed<Return, Args...>().invoke(std::forward<Args>(args)...);
}

template <typename Return, typename... Args>
Return Method::invoke_on(Il2CppObject* self, Args... args) const {
    return typed<Return, Args...>().invoke_on(self, std::forward<Args>(args)...);
}

/**
 * 一次 replacement 回调的上下文。
 *
 * 不可复制/移动，只在当前回调的当前线程有效；`original()` 转发原实现，不经过 runtime_invoke。
 */
template <typename Return, typename... Args>
class ReplacementCall {
public:
    ReplacementCall(const ReplacementCall&) = delete;
    ReplacementCall& operator=(const ReplacementCall&) = delete;
    ReplacementCall(ReplacementCall&&) = delete;
    ReplacementCall& operator=(ReplacementCall&&) = delete;

    /// 本次调用的实例；静态方法为 null。
    [[nodiscard]] Il2CppObject* self() const noexcept { return self_; }

    /// 调用原实现；保留当前实例与隐藏 MethodInfo，可多次显式调用。
    Return original(Args... args) const {
        if (record_ == nullptr || record_->original == nullptr) {
            throw BridgeError(make_error(ErrorCode::scope_violation, ErrorStage::replacement, {},
                                         "original() is only available inside the current replacement callback"));
        }
        if (record_->is_static) {
            auto callable = reinterpret_cast<Return (*)(Args..., const void*)>(record_->original);
            if constexpr (std::is_void_v<Return>) {
                callable(args..., method_);
                return;
            } else {
                return callable(args..., method_);
            }
        }
        auto callable = reinterpret_cast<Return (*)(Il2CppObject*, Args..., const void*)>(record_->original);
        if constexpr (std::is_void_v<Return>) {
            callable(self_, args..., method_);
            return;
        } else {
            return callable(self_, args..., method_);
        }
    }

private:
    template <typename, typename...>
    friend struct detail::ReplacementEntry;

    ReplacementCall(const detail::ReplacementRecordHeader* record, Il2CppObject* self, const void* method) noexcept
        : record_(record), self_(self), method_(method) {}

    const detail::ReplacementRecordHeader* record_{};
    Il2CppObject* self_{};
    const void* method_{};
};

namespace detail {

/// 每个 (Return, Args...) 组合对应的原生替换入口；static 与实例是两种槽形态。
template <typename Return, typename... Args>
struct ReplacementEntry {
    static Return static_thunk(Args... args, const void* method) { return dispatch(nullptr, args..., method); }

    static Return instance_thunk(Il2CppObject* self, Args... args, const void* method) {
        return dispatch(self, args..., method);
    }

private:
    static Return dispatch(Il2CppObject* self, Args... args, const void* method) {
        const auto* record = current_replacement_record();
        if (record == nullptr || record->callback == nullptr) {
            if constexpr (!std::is_void_v<Return>) return Return{};
            else return;
        }

        ReplacementScope scope;
        auto callback = reinterpret_cast<Return (*)(ReplacementCall<Return, Args...>&, Args...)>(record->callback);
        ReplacementCall<Return, Args...> call(record, self, method);
        try {
            if constexpr (std::is_void_v<Return>) {
                callback(call, args...);
                return;
            } else {
                return callback(call, args...);
            }
        } catch (...) {
            /// 用户异常隔离在 native 边界：节流报告并返回默认值，不自动补调用原函数。
            if (record->report_failure != nullptr) record->report_failure(const_cast<ReplacementRecordHeader*>(record));
            if constexpr (!std::is_void_v<Return>) return Return{};
            else return;
        }
    }
};

} /// namespace detail

template <typename Return, typename... Args>
void Method::implementation(Return (*callback)(ReplacementCall<Return, Args...>&, Args...)) const {
    static_assert((detail::has_managed_name<Args>::value && ...),
                  "replacement argument type has no IL2CPP name mapping");
    static_assert(detail::has_managed_name<Return>::value,
                  "replacement result type has no IL2CPP name mapping");
    if (callback == nullptr) {
        throw BridgeError(make_error(ErrorCode::invalid_argument, ErrorStage::replacement, {},
                                     "the replacement callback must not be null"));
    }

    using CleanReturn = std::remove_cvref_t<Return>;
    const std::array<detail::ArgumentSpec, sizeof...(Args)> specifications{
            detail::ArgumentSpec{std::is_pointer_v<std::remove_cvref_t<Args>>,
                                 std::is_same_v<std::remove_cvref_t<Args>, Il2CppString*>,
                                 managed_type_name<Args>}...};
    auto binding = detail::bind_method(context_, generation_, candidates_,
                                       declaring_class_name_ + "::" + name_,
                                       std::is_pointer_v<CleanReturn>,
                                       std::is_same_v<CleanReturn, Il2CppString*>,
                                       managed_type_name<Return>, specifications, true);

    detail::ReplacementRequest request;
    request.signature = std::move(binding);
    request.callback = reinterpret_cast<void*>(callback);
    request.static_entry = reinterpret_cast<void*>(&detail::ReplacementEntry<Return, Args...>::static_thunk);
    request.instance_entry = reinterpret_cast<void*>(&detail::ReplacementEntry<Return, Args...>::instance_thunk);
    detail::install_replacement(request);
}

} /// namespace INB
