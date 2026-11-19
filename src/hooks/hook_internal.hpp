#pragma once

#include "il2cpp_native_bridge/bridge.hpp"
#include "il2cpp_native_bridge/hook_backend.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace INB::detail {

    struct HookState {
        /// 串行化 backend 的移除调用；不保护 original 的读取。
        std::mutex mutex;
        std::shared_ptr<HookBackend> backend;
        void* target{};
        /// 原函数跳板。移除时以 release 置空，转发路径以 acquire 读取，热路径不取锁。
        std::atomic<void*> original{};
        std::atomic_bool removed{};
    };

    /// 按方法句柄解析可安装的原生入口地址。
    Result<void*> resolve_entry_point(const MethodHandle& method);
    /// 按 MethodInfo 解析实现入口；供新门面的 managed replacement 复用。
    Result<void*> resolve_entry_point_address(const Il2CppMethodInfo* method);
    /// 该地址是否被旧 Hook 占用（未移除）。
    bool legacy_hook_active(void* target);
    /// 安装 Hook；地址被占用或后端不可用时返回错误。
    Result<std::shared_ptr<HookState>> install_hook(const MethodHandle& method,
                                                    std::shared_ptr<HookBackend> backend,
                                                    void* replacement);

} /// namespace INB::detail
