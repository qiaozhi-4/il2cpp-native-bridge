#pragma once

#include "detail_types.hpp"
#include "result.hpp"
#include "runtime_state.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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


namespace INB {

    namespace detail {
        struct RuntimeContext;
    }

    class Assembly;
    template <typename T>
    class Array;
    class Class;
    class Domain;
    class Field;
    class GcHandle;
    class Il2Cpp;
    class Image;
    class Method;
    class Object;
    class String;

    /// 公开包装类型的托管名映射；具体类别由 detail::ArgumentSpec 区分。
    template <> struct ManagedTypeName<Object> { static constexpr std::string_view value = "System.Object"; };
    template <> struct ManagedTypeName<String> { static constexpr std::string_view value = "System.String"; };

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
            /// 构造 self<T>() 所需的上下文与声明类信息，由安装时写入。
            std::shared_ptr<RuntimeContext> context;
            std::uint64_t generation{};
            void* declaring_class{};
            std::string declaring_class_name;
            std::string image_name;
            std::string assembly_name;
        };

        /// 当前线程当前 replacement 的记录；不在回调内时为空。由 provider 实现。
        const ReplacementRecordHeader* current_replacement_record();

        /// 首次发现失败时打印 error 日志并抛出；所有门面抛出点统一走这里。
        [[noreturn]] void report_and_throw(ErrorCode code, ErrorStage stage, std::string path, std::string message);

        /// 字段值类型核对：字段托管名与请求的 C++ 类型不一致时，在首次发现处报错。
        void require_field_value_type(std::string_view name, std::string_view actual, std::string_view expected);

        /**
         * 字段读写的类型门槛：只允许按字节可复制的类型，且必须有托管类型名映射。
         *
         * 字段读写按 `sizeof(T)` 落盘，非平凡类型（如 Object、std::string）会把指针写进
         * 对象头部；没有映射的类型无法与字段声明类型比对，两者都必须在编译期挡住。
         */
        template <typename T>
        void check_field_value_type(std::string_view actual, std::string_view name) {
            static_assert(std::is_trivially_copyable_v<T>, "field value type must be trivially copyable");
            static_assert(has_managed_name<T>::value, "field value type has no IL2CPP name mapping");
            if constexpr (has_managed_name<T>::value) {
                require_field_value_type(name, actual, managed_type_name<T>);
            }
        }

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
         * 单参重载：不关心最终结果时使用，等价于 perform(action, {}, {})。
         *
         * action 仍在协调线程异步执行并自动进入运行时访问作用域；失败通过默认诊断报告。
         */
        template <typename Action>
        static Result<void> perform(Action&& action) {
            return perform(std::forward<Action>(action), Completion{}, PerformOptions{});
        }

        /**
         * 带结果与等待诊断的重载：需要观察最终结果或等待阈值时使用。
         *
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
                                    Completion completion,
                                    PerformOptions options) {
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

        /// 在当前作用域内按完整托管名解析类；同名歧义时在首次发现处报错。
        static Class klass(std::string_view full_name);
    };

    /**
     * 已解析类型的不可变描述：保留原生身份与规范托管名。
     *
     * 类别（值/引用）由绑定期按导出能力判定，不在此缓存，避免把不可用能力当成已知事实。
     */
    struct TypeDescriptor {
        /// 借用自 IL2CPP 的类型指针，本结构不拥有。
        void* native{};
        /// 规范化后的托管名，如 System.Int32、UnityEngine.Vector2。
        std::string name;
    };

    /**
     * 已解析方法的不可变描述。
     *
     * 不拥有 runtime context，可在作用域外保存和读取名称/签名；native 指针的有效期由 runtime 卸载决定。
     */
    struct MethodDescriptor {
        /// 借用自 IL2CPP 的 MethodInfo 指针。
        void* native{};
        /// 解析该方法时所在的类；方法继承自基类时与查询类不同。
        void* declaring_class{};
        /// 解析上下文中的类全名，用于拼错误路径与 self<Class>()。
        std::string declaring_class_name;
        /// 方法短名，不含类名。
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

        /// 每个模板实参组合的唯一地址，用作绑定缓存的键；不依赖 RTTI。
        template <typename... Signature>
        const void* signature_tag() {
            static const char tag = 0;
            return &tag;
        }

        /**
         * 复用已校验的绑定。
         *
         * 绑定只取决于「运行时实例 + 代际 + 目标方法 + C++ 签名」，与调用点无关，
         * 因此按这四项共享：每帧新建的 Method 也能命中，不必每次重新核对签名。
         *
         * @param context 句柄所属的 runtime context。
         * @param generation 句柄创建时的代际。
         * @param method 唯一的候选 MethodInfo；多候选时调用方不应查缓存。
         * @param signature `signature_tag` 给出的签名地址。
         * @return 命中时返回共享绑定，未命中返回空。
         */
        std::shared_ptr<const BoundSignature> load_binding(const std::shared_ptr<RuntimeContext>& context,
                                                           std::uint64_t generation,
                                                           const void* method,
                                                           const void* signature);

        /// 记录一次成功的绑定；同一键已存在时保留先写入的那份。
        void save_binding(const std::shared_ptr<RuntimeContext>& context,
                          std::uint64_t generation,
                          const void* method,
                          const void* signature,
                          std::shared_ptr<const BoundSignature> binding);

        /// 一次安装请求：签名绑定结果 + 用户回调 + 两种槽形态的原生入口。
        struct ReplacementRequest {
            BoundSignature signature;
            void* callback{};
            void* static_entry{};
            void* instance_entry{};
            /// self<T>() 构造包装类型所需的声明类身份。
            std::shared_ptr<RuntimeContext> context;
            std::uint64_t generation{};
            void* declaring_class{};
            std::string declaring_class_name;
            std::string image_name;
            std::string assembly_name;
        };

        /// 安装替换；签名不支持、名额耗尽或地址被占用时抛 BridgeError。
        void install_replacement(const ReplacementRequest& request);

        /// 一个 C++ 实参在托管侧的类别。
        struct ArgumentSpec {
            bool reference_carrier{};
            bool string_carrier{};
            std::string_view managed_name;
        };

        /// 类型类别判定：旧裸指针与新包装类型都算引用载体。
        template <typename T>
        struct is_reference_carrier : std::bool_constant<std::is_pointer_v<std::remove_cvref_t<T>>> {};
        template <>
        struct is_reference_carrier<Object> : std::true_type {};
        template <>
        struct is_reference_carrier<String> : std::true_type {};

        template <typename T>
        struct is_string_carrier : std::false_type {};
        template <>
        struct is_string_carrier<String> : std::true_type {};

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
        void* invoke_bound(const BoundSignature& binding, void* instance, std::span<void*> arguments);

        /// 基础类型返回值从装箱对象复制出来；空结果抛 invocation_failed。
        void unbox_result(const BoundSignature& binding, void* boxed, void* destination, std::size_t size);

        /// 字段读写：由包装类型调用，错误在首次发现处报告。
        void field_read(const std::shared_ptr<RuntimeContext>& context,
                        std::uint64_t generation,
                        void* field,
                        void* declaring_class,
                        void* instance,
                        void* destination,
                        std::size_t size,
                        bool is_static);
        void field_write(const std::shared_ptr<RuntimeContext>& context,
                         std::uint64_t generation,
                         void* field,
                         void* declaring_class,
                         void* instance,
                         const void* source,
                         std::size_t size,
                         bool is_static);
        void field_write_boxed(const std::shared_ptr<RuntimeContext>& context,
                               std::uint64_t generation,
                               void* field,
                               void* declaring_class,
                               void* instance,
                               void* boxed,
                               bool is_static);

        /// GCHandle：由 Object::ref/weak_ref 创建，错误在首次发现处报告。
        std::uint32_t gchandle_new(const std::shared_ptr<RuntimeContext>& context,
                                   std::uint64_t generation,
                                   void* target,
                                   bool pinned,
                                   bool weak);
        void gchandle_free(const std::shared_ptr<RuntimeContext>& context, std::uint32_t handle);
        void* gchandle_target(const std::shared_ptr<RuntimeContext>& context, std::uint64_t generation, std::uint32_t handle);

        /// 字符串与数组访问。
        std::string string_content(const std::shared_ptr<RuntimeContext>& context,
                                   std::uint64_t generation,
                                   void* value);
        std::size_t array_length(const std::shared_ptr<RuntimeContext>& context,
                                 std::uint64_t generation,
                                 void* array);
        void* array_element(const std::shared_ptr<RuntimeContext>& context,
                            std::uint64_t generation,
                            void* array,
                            std::size_t index);
        void* unbox_value(const std::shared_ptr<RuntimeContext>& context,
                          std::uint64_t generation,
                          void* boxed);

        /// 视图入口的类型核对：对象类别不符时在首次发现处报错，不做指针重解释。
        void require_string_object(const std::shared_ptr<RuntimeContext>& context,
                                   std::uint64_t generation,
                                   void* object,
                                   std::string_view path);
        void require_array_object(const std::shared_ptr<RuntimeContext>& context,
                                  std::uint64_t generation,
                                  void* object,
                                  std::string_view path);

        /**
         * 实参打包：引用类型传对象指针值，基础类型传值地址。
         */
        void* pack_argument(Object& value) noexcept;
        void* pack_argument(String& value) noexcept;

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
        Return convert_result(const BoundSignature& binding, void* raw) {
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
        Domain(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, void* native)
            : context_(std::move(context)), generation_(generation), native_(native) {}

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
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
                 void* native,
                 std::string name)
            : context_(std::move(context)), generation_(generation), native_(native), name_(std::move(name)) {}

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
        std::string name_;
    };

    class Image {
    public:
        /// 只在该 image 内查找类，不回退到全域同名类。
        [[nodiscard]] Class klass(std::string_view full_name) const;
        [[nodiscard]] std::string_view name() const noexcept { return name_; }

    private:
        friend class Assembly;
        friend class Class;
        Image(std::shared_ptr<detail::RuntimeContext> context,
              std::uint64_t generation,
              void* native,
              std::string name,
              std::string assembly_name)
            : context_(std::move(context)),
              generation_(generation),
              native_(native),
              name_(std::move(name)),
              assembly_name_(std::move(assembly_name)) {}

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
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
        /// 与 JS 库对应的成员命名。
        [[nodiscard]] std::string_view fullName() const noexcept { return full_name_; }
        [[nodiscard]] std::string_view assemblyName() const noexcept { return assembly_name_; }
        [[nodiscard]] std::string_view imageName() const noexcept { return image_name_; }
        [[nodiscard]] Image image() const;
        [[nodiscard]] std::vector<Field> fields() const;
        [[nodiscard]] std::vector<Method> methods() const;
        [[nodiscard]] Field field(std::string_view name) const;
        [[nodiscard]] Object newObject() const;
        [[nodiscard]] std::string toString() const;

    private:
        friend class Image;
        friend class Object;
        friend class Il2Cpp;
        template <typename, typename...>
        friend class ReplacementCall;
        Class(std::shared_ptr<detail::RuntimeContext> context,
              std::uint64_t generation,
              void* native,
              std::string full_name,
              std::string image_name,
              std::string assembly_name);

        Method select(std::string_view name, const std::optional<std::size_t>& parameter_count) const;

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
        std::string full_name_;
        std::string image_name_;
        std::string assembly_name_;
    };

    /**
     * 一个已解析方法的句柄；候选多于一个时先用 overload 收窄。
     *
     * invoke/invoke_on/implementation 都会先绑定静态签名，绑定失败、实例不兼容或托管异常
     * 一律抛 BridgeError，不返回错误码。
     */
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
        Return invoke_on(void* self, Args... args) const;

        /// 在托管对象包装上调用实例方法。
        template <typename Return, typename... Args>
        Return invoke_on(const Object& self, Args... args) const;

        [[nodiscard]] std::string toString() const;

        /// 安装托管方法替换；由库强拥有，不返回需要保活的句柄。
        template <typename Return, typename... Args>
        void implementation(Return (*callback)(ReplacementCall<Return, Args...>&, Args...)) const;

        /// 撤销当前替换；成功后调用回到原实现。仍有调用停留在旧入口时抛 restore_pending，可稍后重试。
        void revert() const;

        /// 当前替换的状态；没有替换时为 not_installed。
        [[nodiscard]] ReplacementState replacement_state() const;

    private:
        friend class Class;
        friend class Object;
        Method(std::shared_ptr<detail::RuntimeContext> context,
               std::uint64_t generation,
               void* declaring_class,
               std::string declaring_class_name,
               std::string name,
               std::vector<MethodDescriptor> candidates,
               void* bound_instance = nullptr,
               std::string image_name = {},
               std::string assembly_name = {});

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* declaring_class_{};
        std::string declaring_class_name_;
        std::string name_;
        std::vector<MethodDescriptor> candidates_;
        void* bound_instance_{};
        std::string image_name_;
        std::string assembly_name_;
    };

    /**
     * 托管字段包装；由 Class（静态字段）或 Object（实例字段）创建。
     *
     * 实例字段绑定了所属对象，get/set 不再需要额外传入实例。
     * get/set 会先核对字段的托管类型名，与 T 不符时抛 BridgeError，不做指针重解释。
     */
    class Field {
    public:
        template <typename T>
        [[nodiscard]] T get() const {
            detail::check_field_value_type<T>(value_type_, name_);
            T value{};
            detail::field_read(context_, generation_, native_, declaring_class_, instance_, &value, sizeof(T), is_static_);
            return value;
        }

        template <typename T>
        void set(const T& value) const {
            detail::check_field_value_type<T>(value_type_, name_);
            detail::field_write(context_, generation_, native_, declaring_class_, instance_, &value, sizeof(T), is_static_);
        }

        template <typename T>
        [[nodiscard]] T get(const Object& instance) const;

        template <typename T>
        void set(const T& value, const Object& instance) const;

        /// 将装箱后的托管值写入字段；用于 Obscured 等值类型字段。
        void set_boxed(Object value) const;

        /// 读取引用类型字段；空值返回默认构造的 Object。
        [[nodiscard]] Object getObject() const;

        [[nodiscard]] std::string_view name() const noexcept { return name_; }
        [[nodiscard]] std::string_view value_type_name() const noexcept { return value_type_; }
        [[nodiscard]] bool isStatic() const noexcept { return is_static_; }
        [[nodiscard]] std::string toString() const;

    private:
        friend class Class;
        friend class Object;
        Field(std::shared_ptr<detail::RuntimeContext> context,
              std::uint64_t generation,
              void* native,
              void* declaring_class,
              void* instance,
              std::string name,
              std::string value_type,
              bool is_static);

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
        void* declaring_class_{};
        void* instance_{};
        std::string name_;
        std::string value_type_;
        bool is_static_{};
    };

    /**
     * 托管对象实例包装；非拥有，不阻止 GC 回收，需要保活时使用 ref()。
     *
     * 类、字段、方法的解析失败都抛 BridgeError，不会返回空的包装对象；对象本身可以为空，
     * 用 isNull() 判断。
     */
    class Object {
    public:
        Object() = default;
        [[nodiscard]] bool isNull() const noexcept { return native_ == nullptr; }
        [[nodiscard]] Class klass() const;
        [[nodiscard]] Field field(std::string_view name) const;
        /// 按名字枚举本类方法候选；找不到抛 BridgeError，多个候选可用 overload 收窄。
        [[nodiscard]] Method method(std::string_view name) const;
        /// 按名字与托管参数个数收窄到唯一候选；找不到抛 BridgeError。
        [[nodiscard]] Method method(std::string_view name, std::size_t managed_parameter_count) const;
        [[nodiscard]] String as_string() const;
        template <typename T>
        [[nodiscard]] Array<T> as_array() const;
        /// 创建强引用 GCHandle 阻止回收；pin 为真时同时固定对象地址。
        [[nodiscard]] GcHandle ref(bool pin) const;
        /// 创建弱引用 GCHandle：不阻止回收，目标被回收后 target() 返回空对象。
        [[nodiscard]] GcHandle weak_ref(bool pin) const;
        [[nodiscard]] std::string toString() const;
        [[nodiscard]] void* native_handle() const noexcept { return native_; }

    private:
        friend class Class;
        friend class Field;
        friend class Method;
        friend class String;
        friend class GcHandle;
        template <typename, typename...>
        friend class ReplacementCall;
        template <typename, typename...>
        friend struct detail::ReplacementEntry;
        template <typename>
        friend class Array;
        template <typename R>
        friend R detail::convert_result(const detail::BoundSignature&, void*);

        Object(std::shared_ptr<detail::RuntimeContext> context,
               std::uint64_t generation,
               void* native,
               void* declaring_class,
               std::string class_name);

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
        void* declaring_class_{};
        std::string class_name_;
    };

    template <typename T>
    T Field::get(const Object& instance) const {
        detail::check_field_value_type<T>(value_type_, name_);
        T value{};
        detail::field_read(context_, generation_, native_, declaring_class_,
                           static_cast<void*>(instance.native_handle()), &value, sizeof(T), is_static_);
        return value;
    }

    template <typename T>
    void Field::set(const T& value, const Object& instance) const {
        detail::check_field_value_type<T>(value_type_, name_);
        detail::field_write(context_, generation_, native_, declaring_class_,
                            static_cast<void*>(instance.native_handle()), &value, sizeof(T), is_static_);
    }

    /**
     * 已校验静态签名的可复用绑定。
     *
     * 保存绑定不保护托管实例存活；每次调用仍核对实例兼容性与非空引用实参的赋值兼容性。
     */
    template <typename Return, typename... Args>
    class BoundMethod {
    public:
        Return invoke(Args... args) const { return invoke_on(nullptr, std::forward<Args>(args)...); }

        Return invoke_on(void* self, Args... args) const {
            auto values = std::tuple<std::remove_cvref_t<Args>...>(std::forward<Args>(args)...);
            auto pointers = detail::pack_arguments(values, std::index_sequence_for<Args...>{});
            void* raw = detail::invoke_bound(
                    *binding_, self, std::span<void*>(pointers.data(), pointers.size()));
            return detail::convert_result<Return>(*binding_, raw);
        }

    private:
        friend class Method;
        explicit BoundMethod(std::shared_ptr<const detail::BoundSignature> binding) : binding_(std::move(binding)) {}

        std::shared_ptr<const detail::BoundSignature> binding_;
    };

    template <typename Return, typename... Args>
    BoundMethod<Return, Args...> Method::typed() const {
        static_assert((detail::has_managed_name<Args>::value && ...),
                      "method argument type has no IL2CPP name mapping");
        static_assert(detail::has_managed_name<Return>::value,
                      "method result type has no IL2CPP name mapping");

        using CleanReturn = std::remove_cvref_t<Return>;
        const void* key = detail::signature_tag<Return, Args...>();

        /// 唯一候选才有确定的绑定键；多候选时 bind 会报歧义，不需要缓存。
        const void* method = candidates_.size() == 1 ? candidates_.front().native : nullptr;
        if (method != nullptr) {
            if (auto cached = detail::load_binding(context_, generation_, method, key); cached != nullptr) {
                return BoundMethod<Return, Args...>(std::move(cached));
            }
        }

        const std::array<detail::ArgumentSpec, sizeof...(Args)> specifications{
                detail::ArgumentSpec{detail::is_reference_carrier<Args>::value,
                                     detail::is_string_carrier<Args>::value,
                                     managed_type_name<Args>}...};
        auto binding = detail::bind_method(context_, generation_, candidates_,
                                           declaring_class_name_ + "::" + name_,
                                           detail::is_reference_carrier<CleanReturn>::value,
                                           detail::is_string_carrier<CleanReturn>::value,
                                           managed_type_name<Return>, specifications);
        auto stored = std::make_shared<const detail::BoundSignature>(std::move(binding));
        if (method != nullptr) {
            detail::save_binding(context_, generation_, method, key, stored);
        }
        return BoundMethod<Return, Args...>(std::move(stored));
    }

    template <typename Return, typename... Args>
    Return Method::invoke(Args... args) const {
        return typed<Return, Args...>().invoke_on(bound_instance_, std::forward<Args>(args)...);
    }

    template <typename Return, typename... Args>
    Return Method::invoke_on(void* self, Args... args) const {
        return typed<Return, Args...>().invoke_on(self, std::forward<Args>(args)...);
    }

    template <typename Return, typename... Args>
    Return Method::invoke_on(const Object& self, Args... args) const {
        return typed<Return, Args...>().invoke_on(
                static_cast<void*>(self.native_handle()), std::forward<Args>(args)...);
    }

    /**
     * 托管 System.String 包装。
     */
    class String {
    public:
        /// 读取 UTF-16 内容并转成 UTF-8；对象为空或缺少字符串导出时抛 BridgeError。
        [[nodiscard]] std::string content() const;
        /// 等于 content().size()，即 UTF-8 字节数，不是 System.String.Length。
        [[nodiscard]] std::size_t length() const { return content().size(); }
        [[nodiscard]] Object object() const;
        [[nodiscard]] void* native_handle() const noexcept { return native_; }
        [[nodiscard]] std::string toString() const { return content(); }

    private:
        friend class Object;
        template <typename, typename...>
        friend struct detail::ReplacementEntry;
        template <typename R>
        friend R detail::convert_result(const detail::BoundSignature&, void*);
        String(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, void* native);

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* native_{};
    };

    /**
     * 托管数组包装；元素类型由调用方声明。
     */
    template <typename T>
    class Array {
    public:
        [[nodiscard]] std::size_t length() const {
            return detail::array_length(context_, generation_, array_);
        }

        /// 按下标取元素；越界抛 BridgeError，值类型按 unbox 后的字节复制。
        [[nodiscard]] T at(std::size_t index) const {
            void* raw = detail::array_element(context_, generation_, array_, index);
            if constexpr (std::is_pointer_v<T>) {
                return static_cast<T>(raw);
            } else {
                T value{};
                void* data = detail::unbox_value(context_, generation_, static_cast<void*>(raw));
                if (data != nullptr) std::memcpy(&value, data, sizeof(T));
                return value;
            }
        }

    private:
        friend class Object;
        Array(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, void* array)
            : context_(std::move(context)), generation_(generation), array_(array) {}

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* array_{};
    };

    /**
     * 托管对象数组的元素视图。
     */
    template <>
    class Array<Object> {
    public:
        [[nodiscard]] std::size_t length() const {
            return detail::array_length(context_, generation_, array_);
        }

        /// 按下标取元素；越界抛 BridgeError。
        [[nodiscard]] Object at(std::size_t index) const {
            auto* raw = static_cast<void*>(detail::array_element(context_, generation_, array_, index));
            return Object(context_, generation_, raw, nullptr, {});
        }

    private:
        friend class Object;
        Array(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, void* array)
            : context_(std::move(context)), generation_(generation), array_(array) {}

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        void* array_{};
    };

    template <typename T>
    Array<T> Object::as_array() const {
        detail::require_array_object(context_, generation_, native_, class_name_);
        return Array<T>(context_, generation_, native_);
    }

    /**
     * RAII GCHandle；析构释放，不跨 runtime 代际使用。
     */
    class GcHandle {
    public:
        GcHandle() = default;
        GcHandle(const GcHandle&) = delete;
        GcHandle& operator=(const GcHandle&) = delete;
        GcHandle(GcHandle&& other) noexcept
            : context_(std::move(other.context_)), generation_(other.generation_), handle_(other.handle_) {
            other.handle_ = 0;
        }
        GcHandle& operator=(GcHandle&& other) noexcept {
            if (this != &other) {
                free();
                context_ = std::move(other.context_);
                generation_ = other.generation_;
                handle_ = other.handle_;
                other.handle_ = 0;
            }
            return *this;
        }
        ~GcHandle() { free(); }

        /// 是否持有一个 GCHandle。
        [[nodiscard]] bool valid() const noexcept { return handle_ != 0; }
        /// 取回目标对象；句柄已释放或目标被回收时返回空对象。
        [[nodiscard]] Object target() const;
        /// 立即释放句柄；重复调用与析构时调用都安全。
        void free() noexcept;

    private:
        friend class Object;
        GcHandle(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, std::uint32_t handle)
            : context_(std::move(context)), generation_(generation), handle_(handle) {}

        std::shared_ptr<detail::RuntimeContext> context_;
        std::uint64_t generation_{};
        std::uint32_t handle_{};
    };

    namespace detail {

        inline void* pack_argument(Object& value) noexcept { return value.native_handle(); }
        inline void* pack_argument(String& value) noexcept { return value.native_handle(); }

        template <>
        inline Object convert_result<Object>(const BoundSignature& binding, void* raw) {
            /// 返回对象的具体类要到运行时才知道，这里只把返回类型名留作 fallback。
            return Object(binding.context, binding.generation, raw,
                          static_cast<void*>(binding.descriptor.declaring_class),
                          binding.descriptor.return_type.name);
        }

        template <>
        inline String convert_result<String>(const BoundSignature& binding, void* raw) {
            return String(binding.context, binding.generation, reinterpret_cast<void*>(raw));
        }

        /// 用户回调参数类型 → native ABI 参数类型。
        template <typename T>
        struct native_argument { using type = T; };
        template <>
        struct native_argument<Object> { using type = void*; };
        template <>
        struct native_argument<String> { using type = void*; };

        template <typename T>
        using native_argument_t = typename native_argument<std::remove_cvref_t<T>>::type;

    } /// namespace detail

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

        /// 兼容旧回调：本次调用的实例裸指针；静态方法为 null。
        [[nodiscard]] void* self() const noexcept { return self_; }

        /// 泛型调用目标：T=INB::Object 返回实例，T=INB::Class 返回声明类。
        /// 不带模板实参时走上面的兼容重载，返回裸指针。
        template <typename T = Object>
        [[nodiscard]] T self() const {
            static_assert(std::is_same_v<T, Object> || std::is_same_v<T, Class>,
                          "self<T> supports only INB::Object or INB::Class");
            if constexpr (std::is_same_v<T, Object>) {
                if (record_ == nullptr || record_->is_static || self_ == nullptr) {
                    detail::report_and_throw(ErrorCode::scope_violation, ErrorStage::replacement, {},
                                             "instance self is unavailable in a static replacement callback");
                }
                return Object(record_->context, record_->generation, self_, record_->declaring_class,
                              record_->declaring_class_name);
            } else {
                if (record_ == nullptr) {
                    detail::report_and_throw(ErrorCode::scope_violation, ErrorStage::replacement, {},
                                             "class self is unavailable outside the current callback");
                }
                return Class(record_->context, record_->generation, record_->declaring_class,
                             record_->declaring_class_name, record_->image_name, record_->assembly_name);
            }
        }

        [[nodiscard]] bool is_static() const noexcept { return record_ != nullptr && record_->is_static; }

        /// 调用原实现；保留当前实例与隐藏 MethodInfo，可多次显式调用。
        Return original(Args... args) const {
            if (record_ == nullptr || record_->original == nullptr) {
                detail::report_and_throw(ErrorCode::scope_violation, ErrorStage::replacement, {},
                                         "original() is only available inside the current replacement callback");
            }
            auto raw_args = std::tuple<detail::native_argument_t<Args>...>{
                    to_native_argument<Args>(args)...};
            return forward_original(raw_args, std::index_sequence_for<Args...>{});
        }

    private:
        template <typename T>
        static detail::native_argument_t<T> to_native_argument(const T& value) {
            if constexpr (std::is_same_v<T, Object>) {
                return static_cast<void*>(value.native_handle());
            } else if constexpr (std::is_same_v<T, String>) {
                return reinterpret_cast<void*>(value.native_handle());
            } else {
                return value;
            }
        }

        template <typename Tuple, std::size_t... Indices>
        Return forward_original(Tuple& raw_args, std::index_sequence<Indices...>) const {
            if (record_->is_static) {
                auto callable = reinterpret_cast<Return (*)(detail::native_argument_t<Args>..., const void*)>(record_->original);
                if constexpr (std::is_void_v<Return>) {
                    callable(std::get<Indices>(raw_args)..., method_);
                    return;
                } else {
                    return callable(std::get<Indices>(raw_args)..., method_);
                }
            }
            auto callable = reinterpret_cast<Return (*)(void*, detail::native_argument_t<Args>..., const void*)>(record_->original);
            if constexpr (std::is_void_v<Return>) {
                callable(self_, std::get<Indices>(raw_args)..., method_);
                return;
            } else {
                return callable(self_, std::get<Indices>(raw_args)..., method_);
            }
        }

    private:
        template <typename, typename...>
        friend struct detail::ReplacementEntry;

        ReplacementCall(const detail::ReplacementRecordHeader* record, void* self, const void* method) noexcept
            : record_(record), self_(self), method_(method) {}

        const detail::ReplacementRecordHeader* record_{};
        void* self_{};
        const void* method_{};
    };

    namespace detail {

        /// 每个 (Return, Args...) 组合对应的原生替换入口；static 与实例是两种槽形态。
        template <typename Return, typename... Args>
        struct ReplacementEntry {
            static Return static_thunk(native_argument_t<Args>... args, const void* method) {
                return dispatch(nullptr, args..., method);
            }

            static Return instance_thunk(void* self, native_argument_t<Args>... args, const void* method) {
                return dispatch(self, args..., method);
            }

        private:
            static Return dispatch(void* self, native_argument_t<Args>... args, const void* method) {
                const auto* record = current_replacement_record();
                if (record == nullptr || record->callback == nullptr) {
                    if constexpr (!std::is_void_v<Return>) return Return{};
                    else return;
                }

                ReplacementScope scope;
                auto callback = reinterpret_cast<Return (*)(ReplacementCall<Return, Args...>&, Args...)>(record->callback);
                ReplacementCall<Return, Args...> call(record, self, method);
                try {
                    auto user_args = std::tuple<Args...>{to_user_argument<Args>(record, args)...};
                    return invoke_callback(callback, call, user_args, std::index_sequence_for<Args...>{});
                } catch (const BridgeError&) {
                    /// 首次发现处已打印过同一条错误，这里只返回默认值，不重复报告。
                    if constexpr (!std::is_void_v<Return>) return Return{};
                    else return;
                } catch (...) {
                    /// 非 BridgeError 的意外异常没有其他报告点，在这里按节流计数上报。
                    if (record->report_failure != nullptr) record->report_failure(const_cast<ReplacementRecordHeader*>(record));
                    if constexpr (!std::is_void_v<Return>) return Return{};
                    else return;
                }
            }

            template <typename T>
            static T to_user_argument(const ReplacementRecordHeader* record, native_argument_t<T> raw) {
                if constexpr (std::is_same_v<T, Object>) {
                    return Object(record->context, record->generation,
                                  static_cast<void*>(raw), record->declaring_class,
                                  record->declaring_class_name);
                } else if constexpr (std::is_same_v<T, String>) {
                    return String(record->context, record->generation, reinterpret_cast<void*>(raw));
                } else {
                    return static_cast<T>(raw);
                }
            }

            template <typename Callback, typename Tuple, std::size_t... Indices>
            static Return invoke_callback(Callback callback, ReplacementCall<Return, Args...>& call,
                                          Tuple& values, std::index_sequence<Indices...>) {
                if constexpr (std::is_void_v<Return>) {
                    callback(call, std::get<Indices>(values)...);
                    return;
                } else {
                    return callback(call, std::get<Indices>(values)...);
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
            detail::report_and_throw(ErrorCode::invalid_argument, ErrorStage::replacement, {},
                                     "the replacement callback must not be null");
        }

        using CleanReturn = std::remove_cvref_t<Return>;
        const std::array<detail::ArgumentSpec, sizeof...(Args)> specifications{
                detail::ArgumentSpec{detail::is_reference_carrier<Args>::value,
                                     detail::is_string_carrier<Args>::value,
                                     managed_type_name<Args>}...};
        auto binding = detail::bind_method(context_, generation_, candidates_,
                                           declaring_class_name_ + "::" + name_,
                                           detail::is_reference_carrier<CleanReturn>::value,
                                           detail::is_string_carrier<CleanReturn>::value,
                                           managed_type_name<Return>, specifications, true);

        detail::ReplacementRequest request;
        request.signature = std::move(binding);
        request.context = context_;
        request.generation = generation_;
        request.declaring_class = declaring_class_;
        request.declaring_class_name = declaring_class_name_;
        request.image_name = image_name_;
        request.assembly_name = assembly_name_;
        request.callback = reinterpret_cast<void*>(callback);
        request.static_entry = reinterpret_cast<void*>(&detail::ReplacementEntry<Return, Args...>::static_thunk);
        request.instance_entry = reinterpret_cast<void*>(&detail::ReplacementEntry<Return, Args...>::instance_thunk);
        detail::install_replacement(request);
    }

} /// namespace INB
