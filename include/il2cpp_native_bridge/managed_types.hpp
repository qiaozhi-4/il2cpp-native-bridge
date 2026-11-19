#pragma once

#include "il2cpp_native_bridge/collections.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <cstddef>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace INB {
namespace detail {

/// 便捷层把底层 Result 的错误提升到 BridgeError，交由 perform 边界统一转换。
template <typename T>
T unwrap_or_throw(Result<T> result) {
    if (!result) detail::report_and_throw(result.error().code, result.error().stage, result.error().path, result.error().message);
    return result.take_value();
}

inline void unwrap_or_throw(Result<void> result) {
    if (!result) detail::report_and_throw(result.error().code, result.error().stage, result.error().path, result.error().message);
}

} /// namespace detail

/**
 * 托管 System.String 的 C++ 风格包装。
 *
 * 托管字符串不可变；`operator+=`/`concat` 会用拼接后的内容创建新的 System.String，
 * 并让当前包装对象重新指向它，原托管字符串不受影响。
 */
class ManagedString {
public:
    /// 包装一个已有的托管字符串；不能为空。
    static ManagedString adopt(Runtime runtime, Il2CppString* value) {
        if (value == nullptr) {
            detail::report_and_throw(ErrorCode::invalid_argument,
                                     ErrorStage::lookup, {}, "managed string must not be null");
        }
        return ManagedString(std::move(runtime), value);
    }

    /// 从 UTF-8 文本创建托管字符串。
    static ManagedString from_utf8(Runtime runtime, std::string_view text) {
        auto value = detail::unwrap_or_throw(runtime.string_from_utf8(text));
        return adopt(std::move(runtime), value);
    }

    /// 从 UTF-16 文本创建托管字符串。
    static ManagedString from_utf16(Runtime runtime, std::u16string_view text) {
        auto value = detail::unwrap_or_throw(runtime.string_from_utf16(text));
        return adopt(std::move(runtime), value);
    }

    /// 以 UTF-8 读取文本内容。
    [[nodiscard]] std::string utf8() const {
        return detail::unwrap_or_throw(runtime_.string_to_utf8(native_));
    }

    /// 以 UTF-16 读取文本内容。
    [[nodiscard]] std::u16string utf16() const {
        return detail::unwrap_or_throw(runtime_.string_to_utf16(native_));
    }

    /// 字符数（UTF-16 code unit 数），与 System.String.Length 一致。
    [[nodiscard]] std::size_t size() const { return utf16().size(); }
    [[nodiscard]] std::size_t length() const { return size(); }
    [[nodiscard]] bool empty() const { return size() == 0; }

    /// 返回原托管字符串指针。
    [[nodiscard]] Il2CppString* native() const noexcept { return native_; }

    /** 追加 UTF-16 文本：创建新字符串并重新绑定当前包装对象。 */
    ManagedString& append(std::u16string_view text) {
        auto combined = utf16();
        combined.append(text.data(), text.size());
        native_ = detail::unwrap_or_throw(runtime_.string_from_utf16(combined));
        return *this;
    }

    /** 追加 UTF-8 文本：先转成托管字符串再按 UTF-16 拼接，避免丢失原始码点。 */
    ManagedString& append(std::string_view text) {
        auto part = detail::unwrap_or_throw(runtime_.string_from_utf8(text));
        auto part_utf16 = detail::unwrap_or_throw(runtime_.string_to_utf16(part));
        return append(part_utf16);
    }

    /// 返回拼接后的新包装对象，当前对象不变。
    [[nodiscard]] ManagedString concat(std::string_view text) const {
        auto result = *this;
        result += text;
        return result;
    }

    [[nodiscard]] ManagedString concat(const ManagedString& other) const {
        auto result = *this;
        result += other.utf16();
        return result;
    }

    ManagedString& operator+=(std::string_view text) { return append(text); }
    ManagedString& operator+=(std::u16string_view text) { return append(text); }
    ManagedString& operator+=(const ManagedString& other) { return append(other.utf16()); }

    [[nodiscard]] bool operator==(const ManagedString& other) const { return utf16() == other.utf16(); }
    [[nodiscard]] bool operator!=(const ManagedString& other) const { return !(*this == other); }
    [[nodiscard]] bool operator==(std::string_view text) const {
        auto value = utf8();
        return std::string_view(value) == text;
    }
    [[nodiscard]] bool operator!=(std::string_view text) const { return !(*this == text); }

    friend ManagedString operator+(ManagedString lhs, std::string_view rhs) {
        lhs += rhs;
        return lhs;
    }
    friend ManagedString operator+(ManagedString lhs, const ManagedString& rhs) {
        lhs += rhs;
        return lhs;
    }
    friend std::ostream& operator<<(std::ostream& stream, const ManagedString& value) {
        return stream << value.utf8();
    }

private:
    ManagedString(Runtime runtime, Il2CppString* value)
        : runtime_(std::move(runtime)), native_(value) {}

    Runtime runtime_;
    Il2CppString* native_{};
};

/**
 * 托管 List<T> 的 C++ 风格包装。
 *
 * 支持 `size()`、`operator[]`、range-for 与 `to_vector()`。
 * 失败以 BridgeError 抛出，在 `Il2Cpp::perform` 边界会被转换成 Result<void>。
 */
template <typename T>
class ManagedList {
public:
    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = T;

        Iterator(const ManagedList* owner, std::size_t index) : owner_(owner), index_(index) {}

        [[nodiscard]] T operator*() const { return owner_->at(index_); }
        Iterator& operator++() {
            ++index_;
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const { return !(*this == other); }

    private:
        const ManagedList* owner_{};
        std::size_t index_{};
    };

    /// 包装已有列表；绑定失败时抛 BridgeError。
    static ManagedList adopt(Runtime runtime, Il2CppObject* value) {
        return ManagedList(ListView<T>::bind(std::move(runtime), value));
    }

    /// 返回 Result 形态的绑定，供需要显式处理错误的调用方使用。
    static Result<ManagedList> bind(Runtime runtime, Il2CppObject* value) {
        auto view = ListView<T>::bind(std::move(runtime), value);
        if (!view.found()) return Result<ManagedList>::failure(view.error());
        return Result<ManagedList>::success(ManagedList(std::move(view)));
    }

    [[nodiscard]] std::size_t size() const { return detail::unwrap_or_throw(view_.size()); }
    [[nodiscard]] std::size_t count() const { return size(); }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] Il2CppObject* native() const noexcept { return view_.native(); }

    /// 按下标读取；越界或读取失败抛 BridgeError。
    [[nodiscard]] T at(std::size_t index) const { return detail::unwrap_or_throw(view_.at(index)); }
    [[nodiscard]] T operator[](std::size_t index) const { return at(index); }
    [[nodiscard]] Result<T> try_at(std::size_t index) const { return view_.at(index); }

    template <typename Callback>
    void for_each(Callback&& callback) const {
        detail::unwrap_or_throw(view_.for_each(std::forward<Callback>(callback)));
    }

    [[nodiscard]] std::vector<T> to_vector() const {
        std::vector<T> result;
        result.reserve(size());
        for_each([&](T item) { result.push_back(std::move(item)); });
        return result;
    }

    [[nodiscard]] Iterator begin() const {
        iteration_size_ = size();
        return Iterator(this, 0);
    }

    [[nodiscard]] Iterator end() const { return Iterator(this, iteration_size_); }

private:
    explicit ManagedList(ListView<T> view) : view_(std::move(view)) {}

    ListView<T> view_;
    mutable std::size_t iteration_size_{};
};

/**
 * 托管字典的 C++ 风格包装。
 *
 * 支持 `size()`、range-for 与 `to_vector()`；range-for 会先把 kv 物化到 vector，
 * 因此遍历期间托管字典发生变化不会影响当前循环。
 */
template <typename K = Il2CppObject*, typename V = Il2CppObject*>
class ManagedDictionary {
public:
    using Pair = std::pair<K, V>;

    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Pair;
        using difference_type = std::ptrdiff_t;
        using pointer = const Pair*;
        using reference = const Pair&;

        Iterator(std::shared_ptr<std::vector<Pair>> items, std::size_t index)
            : items_(std::move(items)), index_(index) {}

        [[nodiscard]] reference operator*() const { return (*items_)[index_]; }
        [[nodiscard]] pointer operator->() const { return &(*items_)[index_]; }
        Iterator& operator++() {
            ++index_;
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const {
            return items_ == other.items_ && index_ == other.index_;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const { return !(*this == other); }

    private:
        std::shared_ptr<std::vector<Pair>> items_;
        std::size_t index_{};
    };

    /// 包装已有字典；绑定失败时抛 BridgeError。
    static ManagedDictionary adopt(Runtime runtime, Il2CppObject* value) {
        return ManagedDictionary(DictionaryView<K, V>::bind(std::move(runtime), value));
    }

    /// 返回 Result 形态的绑定。
    static Result<ManagedDictionary> bind(Runtime runtime, Il2CppObject* value) {
        auto view = DictionaryView<K, V>::bind(std::move(runtime), value);
        if (!view.found()) return Result<ManagedDictionary>::failure(view.error());
        return Result<ManagedDictionary>::success(ManagedDictionary(std::move(view)));
    }

    [[nodiscard]] std::size_t size() const { return detail::unwrap_or_throw(view_.size()); }
    [[nodiscard]] std::size_t count() const { return size(); }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] Il2CppObject* native() const noexcept { return view_.native(); }

    template <typename Callback>
    void for_each(Callback&& callback) const {
        detail::unwrap_or_throw(view_.for_each(std::forward<Callback>(callback)));
    }

    [[nodiscard]] std::vector<Pair> to_vector() const {
        std::vector<Pair> result;
        result.reserve(size());
        for_each([&](K key, V value) {
            result.emplace_back(std::move(key), std::move(value));
        });
        return result;
    }

    [[nodiscard]] Iterator begin() const {
        iteration_items_ = std::make_shared<std::vector<Pair>>(to_vector());
        return Iterator(iteration_items_, 0);
    }

    [[nodiscard]] Iterator end() const {
        static const auto empty = std::make_shared<std::vector<Pair>>();
        auto items = iteration_items_ ? iteration_items_ : empty;
        return Iterator(std::move(items), items->size());
    }

private:
    explicit ManagedDictionary(DictionaryView<K, V> view) : view_(std::move(view)) {}

    DictionaryView<K, V> view_;
    mutable std::shared_ptr<std::vector<Pair>> iteration_items_;
};

} /// namespace INB
