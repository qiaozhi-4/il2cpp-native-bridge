#pragma once

#include "il2cpp_native_bridge/bridge.hpp"
#include "il2cpp_native_bridge/hook_backend.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace il2cpp_native_bridge::detail {

struct HookState {
    /// 串行化 backend 的移除调用；不保护 original 的读取。
    std::mutex mutex;
    std::shared_ptr<HookBackend> backend;
    void* target{};
    /// 原函数跳板。移除时以 release 置空，转发路径以 acquire 读取，热路径不取锁。
    std::atomic<void*> original{};
    std::atomic_bool removed{};
};

Result<void*> resolve_entry_point(const MethodHandle& method);
Result<std::shared_ptr<HookState>> install_hook(const MethodHandle& method,
                                                std::shared_ptr<HookBackend> backend,
                                                void* replacement);

} /// namespace il2cpp_native_bridge::detail
