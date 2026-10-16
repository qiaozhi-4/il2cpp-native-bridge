#pragma once

#include "hook_backend.hpp"

#include <functional>
#include <memory>

namespace il2cpp_native_bridge {

/**
 * 初始化 Frida Gum（进程内只初始化一次）并创建 Hook backend。
 *
 * @return 可用的 backend；无法取得 Gum interceptor 时返回空指针。
 */
std::shared_ptr<HookBackend> make_frida_gum_hook_backend();

/**
 * 在可用的 Frida Gum backend 作用域内同步执行回调。
 *
 * 回调收到的 shared_ptr 引用只在回调期间借用；需要延长 backend 生命周期时应复制 shared_ptr。
 *
 * @param action 要执行的操作，不能为空。
 * @return action 的结果；action 为空时返回 invalid_argument，backend 不可用时返回
 *         hook_backend_unavailable，action 返回的其他错误原样传回。
 * @throws action 抛出的异常会传给调用方。
 */
Result<void> with_frida_gum_hook_backend(
    const std::function<Result<void>(const std::shared_ptr<HookBackend>&)>& action);

} /// namespace il2cpp_native_bridge
