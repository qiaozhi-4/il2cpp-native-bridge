#include "hook_internal.hpp"
#include "hooks/managed_provider_internal.hpp"
#include "metadata/metadata_internal.hpp"

#include <unordered_map>

namespace il2cpp_native_bridge::detail {
namespace {

/// Hook 注册表。
struct HookRegistry {
    std::mutex mutex;
    std::unordered_map<void*, std::weak_ptr<HookState>> active;
};

/**
 * 取得进程内唯一的 Hook 注册表。
 *
 * 实例有意用 new 分配且永不析构。注册表若作为普通静态对象，其析构顺序相对调用方的
 * 静态 HookHandle 未定义；改为普通函数内静态更糟——函数内静态在首次调用时才构造，
 * 必然晚于命名空间静态构造，因而必然先析构，会让退出期访问稳定复现。
 * 只有「永不析构」才能让静态存储期的句柄在进程退出时安全移除。
 */
HookRegistry& registry() {
    static HookRegistry* instance = new HookRegistry;
    return *instance;
}

} /// namespace

bool legacy_hook_active(void* target) {
    if (target == nullptr) return false;
    auto& hooks = registry();
    std::lock_guard registry_lock(hooks.mutex);
    const auto active = hooks.active.find(target);
    if (active == hooks.active.end()) return false;
    const auto state = active->second.lock();
    return state != nullptr && !state->removed.load(std::memory_order_acquire);
}

void* hook_original(const void* state) noexcept {
    if (state == nullptr) return nullptr;
    return static_cast<const HookState*>(state)->original.load(std::memory_order_acquire);
}

Result<std::shared_ptr<HookState>> install_hook(const MethodHandle& method,
                                                std::shared_ptr<HookBackend> backend,
                                                void* replacement) {
    auto valid = validate_method_handle(method);
    if (!valid) return Result<std::shared_ptr<HookState>>::failure(valid.error());
    if (!backend || replacement == nullptr) {
        return Result<std::shared_ptr<HookState>>::failure(make_error(ErrorCode::hook_backend_unavailable,
            "Hook backend or replacement is missing"));
    }
    auto entry = resolve_entry_point(method);
    if (!entry) return Result<std::shared_ptr<HookState>>::failure(entry.error());

    /// 新门面的 managed 替换与旧 Hook 共用地址排他；不夺取彼此的所有权。
    if (managed_replacement_active(entry.value())) {
        return Result<std::shared_ptr<HookState>>::failure(make_error(ErrorCode::owner_conflict,
            "method already has an active managed replacement"));
    }
    auto& hooks = registry();
    std::lock_guard registry_lock(hooks.mutex);
    if (const auto active = hooks.active.find(entry.value()); active != hooks.active.end()) {
        if (auto state = active->second.lock()) {
            if (!state->removed.load(std::memory_order_acquire)) {
                return Result<std::shared_ptr<HookState>>::failure(make_error(ErrorCode::hook_install_failed,
                    "method already has an active Hook"));
            }
        }
        hooks.active.erase(active);
    }

    auto installed = backend->install(entry.value(), replacement);
    if (!installed) {
        return Result<std::shared_ptr<HookState>>::failure(installed.error());
    }
    if (installed.value() == nullptr) {
        return Result<std::shared_ptr<HookState>>::failure(make_error(ErrorCode::hook_install_failed,
            "Hook backend returned a null trampoline"));
    }
    auto state = std::make_shared<HookState>();
    state->backend = std::move(backend);
    state->target = entry.value();
    state->original.store(installed.value(), std::memory_order_release);
    hooks.active[state->target] = state;
    return Result<std::shared_ptr<HookState>>::success(std::move(state));
}

Result<void> remove_hook(const std::shared_ptr<HookState>& state) {
    if (!state) return Result<void>::success();
    std::lock_guard lock(state->mutex);
    if (state->removed.load(std::memory_order_acquire)) return Result<void>::success();
    if (!state->backend || state->target == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::hook_install_failed, "Hook registration is incomplete"));
    }
    auto removed = state->backend->remove(state->target);
    if (!removed) return Result<void>::failure(removed.error());
    /// 先置空跳板再标记已移除，保证读到 removed 时 original 必为 nullptr。
    state->original.store(nullptr, std::memory_order_release);
    state->removed.store(true, std::memory_order_release);
    {
        auto& hooks = registry();
        std::lock_guard registry_lock(hooks.mutex);
        const auto active = hooks.active.find(state->target);
        if (active != hooks.active.end()) {
            auto registered = active->second.lock();
            if (registered == state) hooks.active.erase(active);
        }
    }
    return Result<void>::success();
}

} /// namespace il2cpp_native_bridge::detail
