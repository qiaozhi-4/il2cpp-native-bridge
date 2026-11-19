#pragma once

#include "detail_types.hpp"
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

struct Il2CppArray;
struct Il2CppClass;
struct Il2CppDomain;
struct Il2CppFieldInfo;
struct Il2CppMethodInfo;
struct Il2CppObject;
struct Il2CppString;

namespace INB {

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

    /**
     * 已解析托管类的句柄；按值保存，可安全跨作用域传递。
     *
     * generation 记录解析时的 runtime 代际，消费前须与当前代际核对，代际变化后句柄失效。
     */
    struct ClassHandle {
        /// 共享的 runtime context，同时决定句柄生命周期；为空表示无效句柄。
        std::shared_ptr<detail::RuntimeContext> context;
        /// 借用自 IL2CPP 的原生类指针，不由本句柄拥有，卸载 runtime 后失效。
        Il2CppClass* native{};
        /// 解析时的 runtime 代际。
        std::uint64_t generation{};
        /// 完整托管名，含命名空间。
        std::string full_name;
        /// 该类所属程序集名。
        std::string assembly_name;
    };

    /** 已解析托管方法的句柄，依附于 declaring_class 的有效性。 */
    struct MethodHandle {
        /// 声明该方法的类句柄。
        ClassHandle declaring_class;
        /// 借用自 IL2CPP 的方法元数据指针，不由本句柄拥有。
        const Il2CppMethodInfo* native{};
        /// 方法短名，不含类名。
        std::string name;
        /// 返回类型的托管名。
        std::string return_type;
        /// 各参数类型的托管名，按声明顺序。
        std::vector<std::string> parameter_types;
        /// 是否为静态方法。
        bool is_static{};
    };

    /** 已解析托管字段的句柄，依附于 declaring_class 的有效性。 */
    struct FieldHandle {
        /// 声明该字段的类句柄。
        ClassHandle declaring_class;
        /// 借用自 IL2CPP 的字段元数据指针，不由本句柄拥有。
        const Il2CppFieldInfo* native{};
        /// 字段短名。
        std::string name;
        /// 字段类型的托管名。
        std::string value_type;
        /// 是否为静态字段。
        bool is_static{};
    };

    /** 方法的名称与签名描述，不含原生指针，可在作用域外长期保存。 */
    struct MethodMetadata {
        /// 方法短名。
        std::string name;
        /// 返回类型的托管名。
        std::string return_type;
        /// 各参数类型的托管名，按声明顺序。
        std::vector<std::string> parameter_types;
        /// 是否为静态方法。
        bool is_static{};
    };

    /** 等待 runtime 就绪时的轮询与超时配置。 */
    struct RuntimeWaitOptions {
        /// 就绪状态检查间隔，必须为正数。
        std::chrono::milliseconds poll_interval{100};
        /// 超时阈值；大于 0 时到达后触发一次 on_timeout，0 表示不报超时。
        std::chrono::milliseconds timeout{10000};
        /// 超时时最多调用一次的回调，在后台等待线程执行。
        std::function<void(const RuntimeSnapshot&, std::chrono::milliseconds)> on_timeout;
    };

    /// 托管引用载体与字符串的映射仍由底层保留，供旧 Query/Invoke API 使用。
    template <> struct ManagedTypeName<Il2CppObject*> { static constexpr std::string_view value = "System.Object"; };
    template <> struct ManagedTypeName<Il2CppString*> { static constexpr std::string_view value = "System.String"; };

    /**
     * IL2CPP runtime 的查询入口；实例轻量可复制，共享进程内唯一的运行时 context。
     *
     * 通过 current() 获取实例。该类的方法不会主动附加线程，也未附加线程时多数查询返回
     * runtime_not_ready；需要附加线程时使用 with_attached_thread() 或 perform_when_ready()。
     */
    class Runtime {
    public:
        /**
         * 获取进程内唯一的 runtime 实例，并尝试刷新其就绪状态。
         * @return 绑定到全局 context 的实例；未就绪时仍返回实例，后续操作会返回对应错误。
         */
        static Runtime current();
        /**
         * 注册宿主自定义的 IL2CPP 导出符号解析器，替代默认模块扫描。
         *
         * 该方法只设置解析器，不做初始化等待。必须在任何 runtime 操作之前调用；参数为空或
         * runtime 已进入 ready 时记录诊断并终止进程。
         * 以完全相同的三个参数重复调用无副作用。
         *
         * @param module 宿主提供的模块句柄，不能为空。
         * @param lookup 符号查找函数，不能为空。
         * @param is_ready 可选就绪检查；为空表示模块始终可扫描。
         */
        static void set_symbol_lookup(void* module,
                                      RuntimeSymbolLookup lookup,
                                      RuntimeSymbolLookupReady is_ready = nullptr);
        /**
         * 为单个 IL2CPP 导出注册地址覆盖，优先于符号解析器的结果。
         *
         * 仅能在 runtime 就绪前调用，用于修补扫描器无法定位的导出。
         *
         * @param symbol_name 以 "il2cpp_" 开头的导出符号名。
         * @param address 覆盖用的函数地址，不能为空。
         * @return 符号名或地址非法、runtime 已就绪时返回错误。
         */
        static Result<void> set_export_override(std::string_view symbol_name, void* address);

        /**
         * 返回当前状态快照，读取前会尝试刷新一次。
         * @return 状态快照；context 为空时返回默认构造的快照。
         */
        [[nodiscard]] RuntimeSnapshot snapshot() const;
        /**
         * 主动刷新一次就绪状态。
         * @return 刷新结果；仍等待模块或不受支持时返回对应错误。
         */
        Result<void> refresh() const;

        /**
         * 在后台线程等待 IL2CPP runtime 就绪，再附加该线程并执行 action。
         *
         * waiting_for_module/initializing 状态会持续轮询；超过 options.timeout 时最多调用一次
         * options.on_timeout，然后继续等待。unsupported runtime、线程附加错误或 action 错误
         * 会传给 completion，不会重试 action。该方法返回只表示后台任务已成功启动。
         *
         * @param action 就绪后执行的操作，不能为空。
         * @param completion 任务结束回调，不能为空；action、超时和完成回调均在后台线程执行，
         *                   其捕获对象必须在任务结束前保持有效。启动成功后恰好调用一次。
         * @param options 轮询间隔、超时与超时回调配置。
         * @return 后台任务启动结果；启动失败时直接返回错误且不调用 completion。
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
        /**
         * 枚举当前已加载的托管类。
         * @param name_filter 名称子串过滤条件；为空返回全部，按名称与程序集排序。
         * @return 类查询结果列表；缺少类枚举导出或 runtime 未就绪时返回错误。
         */
        Result<std::vector<ClassQuery>> classes(std::string_view name_filter = {}) const;
        /**
         * 按完整托管名创建延迟解析的类查询。
         * @param fully_qualified_name 含命名空间的完整类名。
         * @return 类查询对象；是否命中需检查其 found()。
         */
        ClassQuery klass(std::string_view fully_qualified_name) const;
        /**
         * 查询托管对象实际所属的类。
         * @param instance 托管对象，不能为空。
         * @return 类查询对象；实例为空或缺少类型识别导出时返回失败结果。
         */
        ClassQuery klass_of(Il2CppObject* instance) const;
        /**
         * 返回给定类查询的直接基类查询。
         *
         * IL2CPP 的元数据枚举不包含继承成员；需要沿继承链查找方法时可用本接口逐级上溯。
         *
         * @param query 已解析的类查询；未命中时原样返回错误。
         * @return 基类查询；无基类、缺少导出或解析失败时返回未命中查询。
         */
        ClassQuery base_class(const ClassQuery& query) const;
        /**
         * 按泛型类型定义名与实参构造封闭泛型类。
         *
         * 依赖反射相关可选导出；缺失时返回失败的类查询对象。
         *
         * @param generic_type_name 泛型定义的完整名，如 "System.Collections.Generic.List`1"。
         * @param type_arguments 各类型实参的完整托管名，不能为空。
         * @param assembly_name 可选的泛型定义所在程序集名。
         * @return 类查询对象；是否命中需检查其 found()。
         */
        ClassQuery generic_class(std::string_view generic_type_name,
                                 std::span<const std::string_view> type_arguments,
                                 std::optional<std::string_view> assembly_name = std::nullopt) const;
        /**
         * 按完整名解析类并返回原生句柄。
         * @param fully_qualified_name 含命名空间的完整类名。
         * @param assembly_name 可选的程序集名，用于消除同名类歧义。
         * @return 类句柄；未找到、同名歧义或 runtime 未就绪时返回错误。
         */
        Result<ClassHandle> class_by_name(std::string_view fully_qualified_name,
                                          std::optional<std::string> assembly_name = std::nullopt) const;

        /**
         * 读取托管数组的元素个数。
         *
         * 优先使用 `il2cpp_array_length`；导出缺失时回退到 `System.Array.get_Length` 反射调用。
         *
         * @param array 托管数组对象，不能为空。
         * @return 元素个数；对象为空、运行时未就绪或导出/反射均不可用时返回错误。
         */
        Result<std::size_t> array_length(Il2CppObject* array) const;
        /**
         * 取得装箱值类型的数据指针，供按字节读取。
         *
         * 返回的指针借用自托管对象，不在本接口内保活；调用期间保持 boxed 有效即可。
         *
         * @param boxed 装箱后的托管值，不能为空。
         * @return 指向值数据的指针；对象为空或导出不可用时返回错误。
         */
        Result<void*> unbox(Il2CppObject* boxed) const;

        /**
         * 把托管字符串读取为 UTF-8。
         * @param value 托管字符串，不能为空。
         * @return UTF-8 文本；缺少字符串读取导出或读取失败时返回错误。
         */
        Result<std::string> string_to_utf8(Il2CppString* value) const;
        /**
         * 把托管字符串读取为 UTF-16。
         * @param value 托管字符串，不能为空。
         * @return UTF-16 文本；缺少字符串读取导出或读取失败时返回错误。
         */
        Result<std::u16string> string_to_utf16(Il2CppString* value) const;
        /**
         * 从 UTF-8 文本分配托管字符串。
         * @param value UTF-8 文本；按完整内容转换，不因内嵌空字符截断。
         * @return 新字符串；缺少 `il2cpp_string_new_utf16` 导出或分配失败时返回错误。
         */
        Result<Il2CppString*> string_from_utf8(std::string_view value) const;
        /**
         * 从 UTF-16 文本分配托管字符串。
         * @param value UTF-16 文本。
         * @return 新字符串；缺少 `il2cpp_string_new_utf16` 导出或分配失败时返回错误。
         */
        Result<Il2CppString*> string_from_utf16(std::u16string_view value) const;

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

    /**
     * 延迟解析的类查询对象，构造时不报错，通过 found()/error() 暴露解析结果。
     *
     * 所有成员访问在未命中时返回空结果或默认值，调用方应先检查 found()。
     */
    class ClassQuery {
    public:
        /**
         * 限定查询到指定程序集；已解析且匹配时复用结果，避免再次全量扫描。
         * @param assembly_name 程序集名，容忍 ".dll" 后缀差异。
         * @return 限定后的类查询对象。
         */
        ClassQuery in_assembly(std::string_view assembly_name) const;
        /// 是否成功解析到类。
        [[nodiscard]] bool found() const noexcept { return result_.has_value(); }
        /// 解析失败时的错误；found() 为真时读取行为由 Result 决定。
        [[nodiscard]] const Error& error() const { return result_.error(); }
        /**
         * 类的完整托管名。
         * @return 命中时为解析结果的完整名，未命中时为请求时传入的名字。
         */
        [[nodiscard]] std::string_view full_name() const noexcept {
            return result_ ? std::string_view(result_.value().full_name) : std::string_view(full_name_);
        }
        /**
         * 类所属程序集名。
         * @return 命中时为结果的程序集名，未命中且曾限定时为请求的程序集名，否则为空。
         */
        [[nodiscard]] std::string_view assembly_name() const noexcept {
            if (result_) return result_.value().assembly_name;
            return assembly_name_ ? std::string_view(*assembly_name_) : std::string_view{};
        }

        /**
         * 按完整 C++ 签名创建方法查询。
         *
         * @tparam Return 方法返回类型，须有托管类型名映射。
         * @tparam Args 方法参数类型，均须有托管类型名映射。
         * @param name 方法短名。
         * @return 方法查询对象。
         */
        template <typename Return, typename... Args>
        MethodQuery<Return, Args...> method(std::string_view name) const;

        /**
         * 按名字与托管参数个数创建方法查询，不做类型名核对。
         *
         * @tparam Return 方法返回类型。
         * @tparam Args 方法参数类型。
         * @param name 方法短名。
         * @param managed_parameter_count 托管参数个数，不含 this 与隐藏 MethodInfo。
         * @return 方法查询对象。
         */
        template <typename Return, typename... Args>
        MethodQuery<Return, Args...> method(std::string_view name, std::size_t managed_parameter_count) const;

        /**
         * 按字段名与类型创建字段查询。
         * @tparam T 字段 C++ 类型，须有托管类型名映射。
         * @param name 字段短名。
         * @return 字段查询对象。
         */
        template <typename T>
        FieldQuery<T> field(std::string_view name) const;

        /**
         * 创建不核对类型的引用类型字段查询，供 System.Object 等字段使用。
         * @param name 字段短名。
         * @return 字段查询对象。
         */
        FieldQuery<Il2CppObject*> object_field(std::string_view name) const;
        /**
         * 创建按值类型读取、以装箱形式写入的字段查询。
         * @param name 字段短名。
         * @return 字段查询对象。
         */
        FieldQuery<Il2CppObject*> boxed_field(std::string_view name) const;
        /**
         * 枚举类的全部方法元数据。
         * @return 方法描述列表；runtime 未就绪或类句柄失效时返回错误。
         */
        Result<std::vector<MethodMetadata>> methods() const;
        /**
         * 枚举类的全部字段句柄。
         * @return 字段句柄列表；runtime 未就绪或类句柄失效时返回错误。
         */
        Result<std::vector<FieldHandle>> fields() const;
        /**
         * 分配该类的一个新托管实例。
         * @return 新实例；缺少 object_new 导出或类句柄失效时返回错误。
         */
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
            /// 是否成功解析到字段。
            [[nodiscard]] bool found() const noexcept { return field_.has_value(); }
            /// 解析失败时的错误。
            [[nodiscard]] const Error& error() const { return field_.error(); }
            /**
             * 字段类型的托管名。
             * @return 命中时为字段类型名，未命中时为空。
             */
            [[nodiscard]] std::string_view value_type_name() const noexcept {
                return field_ ? std::string_view(field_.value().value_type) : std::string_view{};
            }

        protected:
            explicit FieldQueryBase(Result<FieldHandle> field) : field_(std::move(field)) {}
            /// 从实例或静态字段按字节读取数据到 destination。
            Result<void> read_value(Il2CppObject* instance, void* destination, std::size_t size) const;
            /// 按字节将 source 写入实例或静态字段。
            Result<void> write_value(Il2CppObject* instance, const void* source, std::size_t size) const;
            /// 将装箱值拆箱后写入字段。
            Result<void> write_boxed_value(Il2CppObject* boxed_value, Il2CppObject* instance) const;

            Result<FieldHandle> field_;
        };

    } /// namespace detail

    /**
     * 类型化字段访问器，持有解析结果并提供读写操作。
     *
     * @tparam T 字段 C++ 类型，决定读写的字节数与类型名映射。
     */
    template <typename T>
    class FieldQuery : public detail::FieldQueryBase {
    public:
        /**
         * 读取字段值；未命中或读取失败时返回错误。
         * @param instance 实例字段所属对象；静态字段传 nullptr。
         * @return 字段值。
         */
        Result<T> get(Il2CppObject* instance = nullptr) const {
            T value{};
            auto read = read_value(instance, &value, sizeof(T));
            if (!read) return Result<T>::failure(read.error());
            return Result<T>::success(value);
        }

        /**
         * 写入字段值。
         * @param value 要写入的值，按字节复制。
         * @param instance 实例字段所属对象；静态字段传 nullptr。
         * @return 写入结果。
         */
        Result<void> set(const T& value, Il2CppObject* instance = nullptr) const {
            T copy = value;
            return write_value(instance, &copy, sizeof(T));
        }

        /**
         * 将托管装箱值拆箱后写入字段。
         * @param boxed_value 装箱后的托管值，不能为空。
         * @param instance 实例字段所属对象；静态字段传 nullptr。
         * @return 写入结果。
         */
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
            /// 是否成功解析到方法。
            [[nodiscard]] bool found() const noexcept { return method_.has_value(); }
            /// 解析失败时的错误。
            [[nodiscard]] const Error& error() const { return method_.error(); }

        protected:
            explicit MethodQueryBase(Result<MethodHandle> method) : method_(std::move(method)) {}
            /// 调用已解析方法，返回原始托管结果指针。
            Result<Il2CppObject*> invoke_value(Il2CppObject* instance, std::span<void*> arguments) const;
            /// 将装箱结果按字节复制到 destination。
            Result<void> copy_value(Il2CppObject* value, void* destination, std::size_t size) const;
            /// 安装 Hook，成功后返回供 HookHandle 持有的不透明状态。
            Result<std::shared_ptr<HookState>> install_value(std::shared_ptr<HookBackend> backend,
                                                             void* replacement) const;

            Result<MethodHandle> method_;
        };

    } /// namespace detail

    /**
     * 类型化方法查询，持有解析结果并提供调用与 Hook 安装。
     *
     * @tparam Return 方法返回类型，须有托管类型名映射。
     * @tparam Args 方法参数类型，均须有托管类型名映射。
     */
    template <typename Return, typename... Args>
    class MethodQuery : public detail::MethodQueryBase {
    public:
        /**
         * 以静态方法调用；实例方法须使用 invoke_on。
         * @param args 与 Args 一一对应的实参。
         * @return 调用结果；未命中、实例非法或托管异常时返回错误。
         */
        Result<Return> invoke(Args... args) const { return invoke_on(nullptr, std::forward<Args>(args)...); }

        /**
         * 在给定实例上调用方法。
         * @param instance 实例对象；静态方法传 nullptr，实例方法不能为空。
         * @param args 与 Args 一一对应的实参。
         * @return 调用结果；未命中或校验失败时返回错误。
         */
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

        /**
         * 安装静态方法替换。
         * @param backend 提供原生替换能力的后端，按值持有。
         * @param replacement 替换函数，须在 Hook 存续期间保持有效。
         * @return Hook 句柄；安装失败时返回错误。
         */
        Result<HookHandle<Return, Args...>> hook(std::shared_ptr<HookBackend> backend,
                                                 Return (*replacement)(Args...)) const {
            using Handle = HookHandle<Return, Args...>;
            auto installed = install_value(std::move(backend), reinterpret_cast<void*>(replacement));
            if (!installed) return Result<Handle>::failure(installed.error());
            return Result<Handle>::success(Handle(installed.take_value()));
        }

        /**
         * 安装实例方法替换，替换函数首参为实例对象。
         * @param backend 提供原生替换能力的后端，按值持有。
         * @param replacement 替换函数，须在 Hook 存续期间保持有效。
         * @return Hook 句柄；安装失败时返回错误。
         */
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

} /// namespace INB
