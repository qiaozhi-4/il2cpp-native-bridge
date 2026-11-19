#pragma once

#include "il2cpp_native_bridge/bridge.hpp"
#include "il2cpp_native_bridge/result.hpp"

#include <cstddef>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace INB {

    namespace detail {

        /// 把反射返回的 System.Object 转成 C++ 元素类型：引用类型直接转指针，值类型拆箱后按字节复制。
        template <typename T>
        Result<T> from_boxed(const Runtime& runtime, Il2CppObject* boxed) {
            if constexpr (std::is_pointer_v<T>) {
                return Result<T>::success(reinterpret_cast<T>(boxed));
            } else {
                if (boxed == nullptr) {
                    return Result<T>::failure(make_error(ErrorCode::invocation_failed,
                                                         "value-type element was null"));
                }
                auto data = runtime.unbox(boxed);
                if (!data) return Result<T>::failure(data.error());
                T value{};
                std::memcpy(&value, data.value(), sizeof(T));
                return Result<T>::success(value);
            }
        }

    } /// namespace detail

    /**
     * 托管数组的类型化只读视图。
     *
     * 元素访问统一走 `System.Array.GetValue(int)`：引用类型直接得到对象指针，
     * 值类型由 IL2CPP 装箱后按字节取出。数组长度优先使用 `il2cpp_array_length`。
     * 视图只借用托管对象，调用方负责在 GC 期间保持该对象可达。
     *
     * @tparam T 元素类型，可为对象指针（如 Il2CppObject*）或可按字节复制的值类型。
     */
    template <typename T>
    class ArrayView {
    public:
        /**
         * 绑定一个托管数组并解析元素访问所需的反射方法。
         *
         * @param runtime 当前运行时实例。
         * @param array 托管数组对象，不能为空。
         * @return 视图；通过 found()/error() 检查绑定结果。
         */
        static ArrayView bind(Runtime runtime, Il2CppObject* array) {
            ArrayView view(std::move(runtime), array);
            auto array_class = view.runtime_.klass("System.Array");
            if (!array_class.found()) {
                view.binding_ = Result<void>::failure(array_class.error());
                return view;
            }
            auto getter = array_class.template method<Il2CppObject*, std::int32_t>("GetValue");
            if (!getter.found()) {
                view.binding_ = Result<void>::failure(getter.error());
                return view;
            }
            view.getter_ = std::move(getter);
            return view;
        }

        /** @overload */
        static ArrayView bind(Runtime runtime, Il2CppArray* array) {
            return bind(std::move(runtime), reinterpret_cast<Il2CppObject*>(array));
        }

        /// 绑定是否成功；绑定失败时所有操作返回同一错误。
        [[nodiscard]] bool found() const noexcept { return binding_.has_value(); }
        /// 绑定失败时的错误；绑定成功时读取行为由 Result 决定。
        [[nodiscard]] const Error& error() const { return binding_.error(); }
        /// 视图指向的托管数组。
        [[nodiscard]] Il2CppObject* native() const noexcept { return array_; }

        /**
         * 读取元素个数。
         * @return 元素个数；数组为空或绑定失败时返回错误。
         */
        [[nodiscard]] Result<std::size_t> size() const {
            if (!binding_) return Result<std::size_t>::failure(binding_.error());
            return runtime_.array_length(array_);
        }

        /**
         * 按下标读取元素；越界返回错误。
         * @param index 从 0 开始的下标。
         * @return 元素值；引用类型可为 nullptr。
         */
        [[nodiscard]] Result<T> at(std::size_t index) const {
            auto count = size();
            if (!count) return Result<T>::failure(count.error());
            if (index >= count.value()) {
                return Result<T>::failure(make_error(ErrorCode::invalid_argument,
                                                     "array index is out of range"));
            }
            return at_unchecked(index);
        }

        /**
         * 依次把元素交给回调。
         *
         * 回调返回 void 时遍历全部元素；返回 bool 时，返回 false 可提前结束。
         * 回调在当前线程同步执行，允许捕获局部状态。
         *
         * @param callback 签名为 `void(T)` 或 `bool(T)` 的可调用对象。
         * @return 遍历结果；任一元素读取失败时立即返回对应错误。
         */
        template <typename Callback>
        Result<void> for_each(Callback&& callback) const {
            using Return = std::invoke_result_t<Callback&, T>;
            static_assert(std::is_void_v<Return> || std::is_same_v<Return, bool>,
                          "array callback must return void or bool");
            auto count = size();
            if (!count) return Result<void>::failure(count.error());
            for (std::size_t index = 0; index < count.value(); ++index) {
                auto element = at_unchecked(index);
                if (!element) return Result<void>::failure(element.error());
                if constexpr (std::is_same_v<Return, bool>) {
                    if (!callback(element.value())) break;
                } else {
                    callback(element.value());
                }
            }
            return Result<void>::success();
        }

    private:
        ArrayView(Runtime runtime, Il2CppObject* array)
            : runtime_(std::move(runtime)), array_(array) {}

        [[nodiscard]] Result<T> at_unchecked(std::size_t index) const {
            if (!getter_) {
                return Result<T>::failure(make_error(ErrorCode::internal_error,
                                                     "array element accessor is unavailable"));
            }
            auto boxed = getter_->invoke_on(array_, static_cast<std::int32_t>(index));
            if (!boxed) return Result<T>::failure(boxed.error());
            return detail::from_boxed<T>(runtime_, boxed.value());
        }

        Runtime runtime_;
        Il2CppObject* array_{};
        Result<void> binding_{Result<void>::success()};
        std::optional<MethodQuery<Il2CppObject*, std::int32_t>> getter_;
    };

    /**
     * 托管 List<T> 的类型化只读视图。
     *
     * 从对象的具体类开始查找 `get_Count` 与 `get_Item(int)`，并沿继承链上溯，
     * 不依赖 `_items` 字段布局，也不需要调用方提供 T 的托管名。
     *
     * @tparam T 元素类型，可为对象指针（如 Il2CppString*）或可按字节复制的值类型。
     */
    template <typename T>
    class ListView {
    public:
        /**
         * 绑定一个托管 List<T> 并解析计数与下标访问方法。
         *
         * @param runtime 当前运行时实例。
         * @param list 托管列表对象，不能为空。
         * @return 视图；通过 found()/error() 检查绑定结果。
         */
        static ListView bind(Runtime runtime, Il2CppObject* list) {
            ListView view(std::move(runtime), list);
            auto list_class = view.runtime_.klass_of(list);
            std::optional<MethodQuery<std::int32_t>> count;
            std::optional<MethodQuery<Il2CppObject*, std::int32_t>> getter;
            while (list_class.found()) {
                auto candidate_count = list_class.template method<std::int32_t>("get_Count");
                auto candidate_getter = list_class.template method<Il2CppObject*, std::int32_t>("get_Item", 1);
                if (candidate_count.found() && candidate_getter.found()) {
                    count = std::move(candidate_count);
                    getter = std::move(candidate_getter);
                    break;
                }
                list_class = view.runtime_.base_class(list_class);
            }
            if (!count || !getter) {
                view.binding_ = Result<void>::failure(list_class.found()
                        ? list_class.error()
                        : make_error(ErrorCode::not_found, "list get_Count/get_Item not found"));
                return view;
            }
            view.count_ = std::move(count);
            view.getter_ = std::move(getter);
            return view;
        }

        /// 绑定是否成功；绑定失败时所有操作返回同一错误。
        [[nodiscard]] bool found() const noexcept { return binding_.has_value(); }
        /// 绑定失败时的错误。
        [[nodiscard]] const Error& error() const { return binding_.error(); }
        /// 视图指向的托管列表。
        [[nodiscard]] Il2CppObject* native() const noexcept { return list_; }

        /**
         * 读取列表元素个数。
         * @return 元素个数；列表为空或绑定失败时返回错误。
         */
        [[nodiscard]] Result<std::size_t> size() const {
            if (!binding_) return Result<std::size_t>::failure(binding_.error());
            if (list_ == nullptr) {
                return Result<std::size_t>::failure(make_error(ErrorCode::invalid_argument,
                                                               "managed list must not be null"));
            }
            auto count = count_->invoke_on(list_);
            if (!count) return Result<std::size_t>::failure(count.error());
            if (count.value() < 0) {
                return Result<std::size_t>::failure(make_error(ErrorCode::invocation_failed,
                                                               "managed list returned a negative count"));
            }
            return Result<std::size_t>::success(static_cast<std::size_t>(count.value()));
        }

        /**
         * 按下标读取元素；越界返回错误。
         * @param index 从 0 开始的下标。
         * @return 元素值；引用类型可为 nullptr。
         */
        [[nodiscard]] Result<T> at(std::size_t index) const {
            auto count = size();
            if (!count) return Result<T>::failure(count.error());
            if (index >= count.value()) {
                return Result<T>::failure(make_error(ErrorCode::invalid_argument,
                                                     "list index is out of range"));
            }
            return at_unchecked(index);
        }

        /**
         * 依次把元素交给回调。
         *
         * 回调返回 void 时遍历全部元素；返回 bool 时，返回 false 可提前结束。
         *
         * @param callback 签名为 `void(T)` 或 `bool(T)` 的可调用对象。
         * @return 遍历结果；任一元素读取失败时立即返回对应错误。
         */
        template <typename Callback>
        Result<void> for_each(Callback&& callback) const {
            using Return = std::invoke_result_t<Callback&, T>;
            static_assert(std::is_void_v<Return> || std::is_same_v<Return, bool>,
                          "list callback must return void or bool");
            auto count = size();
            if (!count) return Result<void>::failure(count.error());
            for (std::size_t index = 0; index < count.value(); ++index) {
                auto element = at_unchecked(index);
                if (!element) return Result<void>::failure(element.error());
                if constexpr (std::is_same_v<Return, bool>) {
                    if (!callback(element.value())) break;
                } else {
                    callback(element.value());
                }
            }
            return Result<void>::success();
        }

    private:
        ListView(Runtime runtime, Il2CppObject* list)
            : runtime_(std::move(runtime)), list_(list) {}

        [[nodiscard]] Result<T> at_unchecked(std::size_t index) const {
            auto boxed = getter_->invoke_on(list_, static_cast<std::int32_t>(index));
            if (!boxed) return Result<T>::failure(boxed.error());
            return detail::from_boxed<T>(runtime_, boxed.value());
        }

        Runtime runtime_;
        Il2CppObject* list_{};
        Result<void> binding_{Result<void>::success()};
        std::optional<MethodQuery<std::int32_t>> count_;
        std::optional<MethodQuery<Il2CppObject*, std::int32_t>> getter_;
    };

    /**
     * 托管字典的类型化只读遍历视图。
     *
     * 按目标运行时里实际存在的非泛型接口实现遍历：
     * `System.Collections.IDictionary.GetEnumerator()` 返回装箱的枚举器，
     * 再通过 `System.Collections.IDictionaryEnumerator` 的 `MoveNext/get_Key/get_Value`
     * 逐项读取。这样不依赖 `Dictionary<K,V>` 的字段布局，也不要求调用方提供泛型实参名。
     *
     * @tparam K 键的 C++ 表示，可为对象指针或可按字节复制的值类型。
     * @tparam V 值的 C++ 表示，可为对象指针或可按字节复制的值类型。
     */
    template <typename K = Il2CppObject*, typename V = Il2CppObject*>
    class DictionaryView {
    public:
        /**
         * 绑定一个托管字典并解析计数与枚举所需的接口方法。
         *
         * @param runtime 当前运行时实例。
         * @param dictionary 实现 System.Collections.IDictionary 的托管对象，不能为空。
         * @return 视图；通过 found()/error() 检查绑定结果。
         */
        static DictionaryView bind(Runtime runtime, Il2CppObject* dictionary) {
            DictionaryView view(std::move(runtime), dictionary);
            /// IL2CPP 的 class_get_methods 不含继承成员，也没有可用的接口分发调用路径；
            /// 因此从对象的具体类开始逐级回溯，找到直接声明 get_Count/GetEnumerator 的类。
            auto dictionary_class = view.runtime_.klass_of(dictionary);
            std::optional<MethodQuery<std::int32_t>> count;
            std::optional<MethodQuery<Il2CppObject*>> enumerator;
            while (dictionary_class.found()) {
                auto candidate_count = dictionary_class.template method<std::int32_t>("get_Count");
                auto candidate_enumerator = dictionary_class.template method<Il2CppObject*>("GetEnumerator", 0);
                if (candidate_count.found() && candidate_enumerator.found()) {
                    count = std::move(candidate_count);
                    enumerator = std::move(candidate_enumerator);
                    break;
                }
                dictionary_class = view.runtime_.base_class(dictionary_class);
            }
            if (!count || !enumerator) {
                view.binding_ = Result<void>::failure(dictionary_class.found()
                        ? dictionary_class.error()
                        : make_error(ErrorCode::not_found, "dictionary get_Count/GetEnumerator not found"));
                return view;
            }
            view.count_ = std::move(count);
            view.get_enumerator_ = std::move(enumerator);
            return view;
        }

        /// 绑定是否成功；绑定失败时所有操作返回同一错误。
        [[nodiscard]] bool found() const noexcept { return binding_.has_value(); }
        /// 绑定失败时的错误。
        [[nodiscard]] const Error& error() const { return binding_.error(); }
        /// 视图指向的托管字典。
        [[nodiscard]] Il2CppObject* native() const noexcept { return dictionary_; }

        /**
         * 读取字典中的键值对数量。
         * @return 键值对数量；字典为空或绑定失败时返回错误。
         */
        [[nodiscard]] Result<std::size_t> size() const {
            if (!binding_) return Result<std::size_t>::failure(binding_.error());
            if (dictionary_ == nullptr) {
                return Result<std::size_t>::failure(make_error(ErrorCode::invalid_argument,
                                                               "managed dictionary must not be null"));
            }
            auto count = count_->invoke_on(dictionary_);
            if (!count) return Result<std::size_t>::failure(count.error());
            if (count.value() < 0) {
                return Result<std::size_t>::failure(make_error(ErrorCode::invocation_failed,
                                                               "managed dictionary returned a negative count"));
            }
            return Result<std::size_t>::success(static_cast<std::size_t>(count.value()));
        }

        /**
         * 依次把键值交给回调。
         *
         * 回调返回 void 时遍历全部条目；返回 bool 时，返回 false 可提前结束。
         * 枚举器是装箱的值类型，接口方法直接接收该装箱对象，由 IL2CPP 完成接口分发。
         *
         * @param callback 签名为 `void(K, V)` 或 `bool(K, V)` 的可调用对象。
         * @return 遍历结果；任一步骤失败时立即返回对应错误。
         */
        template <typename Callback>
        Result<void> for_each(Callback&& callback) const {
            using Return = std::invoke_result_t<Callback&, K, V>;
            static_assert(std::is_void_v<Return> || std::is_same_v<Return, bool>,
                          "dictionary callback must return void or bool");
            if (!binding_) return Result<void>::failure(binding_.error());
            if (dictionary_ == nullptr) {
                return Result<void>::failure(make_error(ErrorCode::invalid_argument,
                                                        "managed dictionary must not be null"));
            }

            auto boxed_enumerator = get_enumerator_->invoke_on(dictionary_);
            if (!boxed_enumerator) return Result<void>::failure(boxed_enumerator.error());
            Il2CppObject* enumerator = boxed_enumerator.value();
            if (enumerator == nullptr) {
                return Result<void>::failure(make_error(ErrorCode::invocation_failed,
                                                        "dictionary enumerator was null"));
            }

            /// 枚举器类型只在运行时才知道；同样从具体类回溯解析 MoveNext/get_Key/get_Value。
            auto enumerator_class = runtime_.klass_of(enumerator);
            std::optional<MethodQuery<bool>> move_next;
            std::optional<MethodQuery<Il2CppObject*>> get_key;
            std::optional<MethodQuery<Il2CppObject*>> get_value;
            while (enumerator_class.found()) {
                auto candidate_move = enumerator_class.template method<bool>("MoveNext");
                auto candidate_key = enumerator_class.template method<Il2CppObject*>("get_Key");
                if (!candidate_key.found()) {
                    candidate_key = enumerator_class.template method<Il2CppObject*>(
                            "System.Collections.IDictionaryEnumerator.get_Key");
                }
                auto candidate_value = enumerator_class.template method<Il2CppObject*>("get_Value");
                if (!candidate_value.found()) {
                    candidate_value = enumerator_class.template method<Il2CppObject*>(
                            "System.Collections.IDictionaryEnumerator.get_Value");
                }
                if (candidate_move.found() && candidate_key.found() && candidate_value.found()) {
                    move_next = std::move(candidate_move);
                    get_key = std::move(candidate_key);
                    get_value = std::move(candidate_value);
                    break;
                }
                enumerator_class = runtime_.base_class(enumerator_class);
            }
            if (!move_next || !get_key || !get_value) {
                return Result<void>::failure(enumerator_class.found()
                        ? enumerator_class.error()
                        : make_error(ErrorCode::not_found, "enumerator MoveNext/get_Key/get_Value not found"));
            }

            /// 枚举期间保持装箱对象可达；IL2CPP 使用非移动式 GC。
            while (true) {
                auto moved = move_next->invoke_on(enumerator);
                if (!moved) return Result<void>::failure(moved.error());
                if (!moved.value()) break;

                auto boxed_key = get_key->invoke_on(enumerator);
                if (!boxed_key) return Result<void>::failure(boxed_key.error());
                auto boxed_value = get_value->invoke_on(enumerator);
                if (!boxed_value) return Result<void>::failure(boxed_value.error());

                auto key = detail::from_boxed<K>(runtime_, boxed_key.value());
                if (!key) return Result<void>::failure(key.error());
                auto value = detail::from_boxed<V>(runtime_, boxed_value.value());
                if (!value) return Result<void>::failure(value.error());

                if constexpr (std::is_same_v<Return, bool>) {
                    if (!callback(key.value(), value.value())) break;
                } else {
                    callback(key.value(), value.value());
                }
            }
            return Result<void>::success();
        }

    private:
        DictionaryView(Runtime runtime, Il2CppObject* dictionary)
            : runtime_(std::move(runtime)), dictionary_(dictionary) {}

        Runtime runtime_;
        Il2CppObject* dictionary_{};
        Result<void> binding_{Result<void>::success()};
        std::optional<MethodQuery<std::int32_t>> count_;
        std::optional<MethodQuery<Il2CppObject*>> get_enumerator_;
    };

} /// namespace INB
