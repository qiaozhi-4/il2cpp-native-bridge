#include "hook_internal.hpp"
#include "metadata/metadata_internal.hpp"

#include <unordered_map>

namespace il2cpp_native_bridge::detail {
namespace {

std::mutex registry_mutex;
std::unordered_map<void*, std::weak_ptr<HookState>> active_hooks;

} /// namespace

void* original_function(const std::shared_ptr<HookState>& state) noexcept {
    if (!state) return nullptr;
    std::lock_guard lock(state->mutex);
    return state->removed.load(std::memory_order_acquire) ? nullptr : state->original;
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
    std::lock_guard registry_lock(registry_mutex);
    if (const auto active = active_hooks.find(entry.value()); active != active_hooks.end()) {
        if (auto state = active->second.lock()) {
            if (!state->removed.load(std::memory_order_acquire)) {
                return Result<std::shared_ptr<HookState>>::failure(make_error(ErrorCode::hook_install_failed,
                    "method already has an active Hook"));
            }
        }
        active_hooks.erase(active);
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
    state->original = installed.value();
    active_hooks[state->target] = state;
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
    state->removed.store(true, std::memory_order_release);
    state->original = nullptr;
    {
        std::lock_guard registry_lock(registry_mutex);
        const auto active = active_hooks.find(state->target);
        if (active != active_hooks.end()) {
            auto registered = active->second.lock();
            if (registered == state) active_hooks.erase(active);
        }
    }
    return Result<void>::success();
}

} /// namespace il2cpp_native_bridge::detail
