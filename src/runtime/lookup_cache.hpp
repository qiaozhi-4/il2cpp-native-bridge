#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace INB::detail {

    /**
     * 带容量上限与代际失效的只读元数据缓存。
     *
     * 库里所有「解析一次就固定」的查表都走这一个实现：一个表、一把锁、一个上限，
     * 超过上限或代际变化时整体清空，避免长驻进程按查询次数无界增长。
     *
     * 命中返回副本而不是引用：调用方拿到的值不随后续写入失效，也不需要一直持锁。
     *
     * @tparam Key 键类型。
     * @tparam Value 值类型，必须可复制。
     * @tparam Map 底层容器；字符串键用 unordered_map（默认），元组键用 map（无 std::hash）。
     */
    template <typename Key, typename Value, typename Map = std::unordered_map<Key, Value>>
    class LookupCache {
    public:
        explicit LookupCache(std::size_t capacity) : capacity_(capacity) {}

        LookupCache(const LookupCache&) = delete;
        LookupCache& operator=(const LookupCache&) = delete;

        /// 命中返回副本，未命中返回空。
        [[nodiscard]] std::optional<Value> find(const Key& key) const {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found == entries_.end()) return std::nullopt;
            return found->second;
        }

        /// 写入或覆盖一条；达到上限时先整体清空。键与值按值传入，便于调用方 std::move。
        void insert(Key key, Value value) {
            std::lock_guard lock(mutex_);
            if (entries_.size() >= capacity_) entries_.clear();
            entries_.insert_or_assign(std::move(key), std::move(value));
        }

        /**
         * 对齐当前代际；代际变化时清空并记录新代际。
         *
         * 不属于 runtime context 的缓存（例如按方法共享的绑定）用它做失效，
         * 与 context 自带的「重载时清空」保持同一套语义。
         *
         * @return 本次调用是否发生了清理。
         */
        bool advance(std::uint64_t generation) {
            std::lock_guard lock(mutex_);
            if (generation_ == generation) return false;
            generation_ = generation;
            entries_.clear();
            return true;
        }

        void clear() {
            std::lock_guard lock(mutex_);
            entries_.clear();
        }

        [[nodiscard]] std::size_t size() const {
            std::lock_guard lock(mutex_);
            return entries_.size();
        }

    private:
        mutable std::mutex mutex_;
        Map entries_;
        std::size_t capacity_;
        std::uint64_t generation_{};
    };

} /// namespace INB::detail
