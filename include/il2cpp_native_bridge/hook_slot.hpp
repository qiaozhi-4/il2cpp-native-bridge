#pragma once

#include "bridge.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

namespace INB {

/**
 * 管理单个类型化 Hook，并在入口发布与 Hook 启停之间同步，避免替换函数过早读到空原函数。
 *
 * **替换函数必须自行判空**：Gum 可能在 install() 返回前启用替换函数，此时 original() 仍是
 * nullptr；移除成功后同样会清空入口。两种情况都应直接返回，而不是通过空指针转发。
 *
 * @tparam Return 被 Hook 函数的返回类型。
 * @tparam Args 被 Hook 函数的参数类型。
 */
template <typename Return, typename... Args>
class HookSlot {
public:
    using Handle = HookHandle<Return, Args...>;
    using Function = typename Handle::Function;

    HookSlot() = default;
    HookSlot(const HookSlot&) = delete;
    HookSlot& operator=(const HookSlot&) = delete;

    ~HookSlot() { (void) remove(); }

    /**
     * 幂等安装 Hook；持锁执行安装器，确保 Hook 启用前原函数入口已发布。
     *
     * @tparam Installer 返回 Hook 安装结果的可调用类型。
     * @param installer 执行 Hook 安装并返回类型化句柄的可调用对象。
     * @return 安装成功或已经安装时返回成功，否则返回安装错误。
     */
    template <typename Installer>
    Result<void> install(Installer&& installer) {
        std::lock_guard lock(mutex_);
        if (handle_.has_value()) return Result<void>::success();

        /// Gum 可能在 install() 返回前启用替换函数；持锁直到原函数入口发布。
        auto installed = std::forward<Installer>(installer)();
        if (!installed) return Result<void>::failure(installed.error());

        auto handle = installed.take_value();
        const auto original = handle.original();
        if (original == nullptr) {
            return Result<void>::failure(make_error(ErrorCode::hook_install_failed,
                "Hook returned a null original function"));
        }

        handle_.emplace(std::move(handle));
        original_.store(original, std::memory_order_release);
        return Result<void>::success();
    }

    /**
     * 返回已发布的原函数入口；安装或移除竞争期间通过互斥锁确认最终状态。
     *
     * @return 已发布的原函数入口；尚未安装或已移除时返回 nullptr。
     */
    [[nodiscard]] Function original() const {
        auto function = original_.load(std::memory_order_acquire);
        if (function != nullptr) return function;

        std::lock_guard lock(mutex_);
        return original_.load(std::memory_order_acquire);
    }

    /**
     * 移除 Hook，并在成功后清空原函数入口；移除失败时保留当前句柄供后续重试。
     *
     * @return 移除成功或当前未安装时返回成功，否则返回移除错误。
     */
    Result<void> remove() {
        std::lock_guard lock(mutex_);
        if (!handle_.has_value()) return Result<void>::success();

        auto removed = handle_->remove();
        if (!removed) return removed;

        handle_.reset();
        original_.store(nullptr, std::memory_order_release);
        return Result<void>::success();
    }

private:
    /// 串行化安装、移除和原函数入口的兜底读取。
    mutable std::mutex mutex_;
    /// 当前由此槽位管理的 Hook 句柄；无值表示尚未安装。
    std::optional<Handle> handle_;
    /// 已发布的原函数入口，供替换函数无锁读取。
    std::atomic<Function> original_{nullptr};
};

} /// namespace INB
