#pragma once

#include "il2cpp_native_bridge/hook_backend.hpp"

#include <mutex>
#include <atomic>

namespace il2cpp_native_bridge::detail {

struct HookState {
    std::mutex mutex;
    std::shared_ptr<HookBackend> backend;
    void* target{};
    void* original{};
    std::atomic_bool removed{};
};

} /// namespace il2cpp_native_bridge::detail
